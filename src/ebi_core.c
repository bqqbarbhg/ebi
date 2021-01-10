#include "ebi_core.h"

#include <stdlib.h>
#include <string.h>

#include <intrin.h>
#include <Windows.h>

// -- OS abstraction

// Platform

// Intrinsics

bool ebi_dcas(uintptr_t *dst, uintptr_t *cmp, uintptr_t lo, uintptr_t hi)
{
#if defined(_M_X64)
	return (bool)_InterlockedCompareExchange128((volatile long long*)dst,
		(long long)hi, (long long) lo, (long long*)cmp);
#else
	long long r = (long long)cmp[1] << 32 | (long long)cmp[0];
	long long v = _InterlockedCompareExchange64((volatile long long*)dst,
		(long long)hi << 32 | (long long)lo, r);
	cmp[0] = (uintptr_t)r;
	cmp[1] = (uintptr_t)(r >> 32);
	return r == v;
#endif
}

// -- Synchronization

// Intrusive atomic stack

typedef struct ebi_ia_stack {
	uintptr_t v[2];
} ebi_ia_stack;

void ebi_ia_push(ebi_ia_stack *s, void *ptr)
{
	ebi_ia_stack r = *s;
	do {
		*(void**)ptr = (void*)r.v[0];
	} while (!ebi_dcas(s->v, r.v, (uintptr_t)ptr, r.v[1] + 1));
}

void ebi_ia_push_all(ebi_ia_stack *s, void *ptr)
{
	ebi_ia_stack r = *s;
	do {
		ebi_assert(r.v[0] == 0);
	} while (!ebi_dcas(s->v, r.v, (uintptr_t)ptr, r.v[1] + 1));
}

void *ebi_ia_pop(ebi_ia_stack *s)
{
	ebi_ia_stack r = *s;
	void **head;
	do {
		head = (void**)r.v[0];
		if (!head) return NULL;
	} while (!ebi_dcas(s->v, r.v, (uintptr_t)*head, r.v[1] + 1));
	*head = NULL;
	return head;
}

bool ebi_ia_maybe_nonempty(ebi_ia_stack *s)
{
	return s->v[0] != 0;
}

void *ebi_ia_pop_all(ebi_ia_stack *s)
{
	ebi_ia_stack r = *s;
	void **head;
	do {
		head = (void**)r.v[0];
	} while (!ebi_dcas(s->v, r.v, 0, r.v[1] + 1));
	return head;
}

uintptr_t ebi_ia_get_count(ebi_ia_stack *s)
{
	return (uintptr_t)_InterlockedExchangeAdd((volatile long*)&s->v[1], 0);
}

// Mutex

// TODO: Implement this internally
#pragma comment(lib, "Synchronization.lib")

typedef struct ebi_mutex {
	uint32_t lock;
	uint32_t waiters;
} ebi_mutex;

void ebi_mutex_lock(ebi_mutex *m)
{
	uint32_t cmp = 1;
	uint32_t spin = 0;
	for (;;) {
		if (!_interlockedbittestandset((volatile long*)&m->lock, 0)) break;
		if (++spin > 1000) {
			_InterlockedIncrement((volatile long*)&m->waiters);
			WaitOnAddress(&m->lock, &cmp, 4, INFINITE);
			_InterlockedDecrement((volatile long*)&m->waiters);
		}
	}
}

bool ebi_mutex_try_lock(ebi_mutex *m)
{
	return !_interlockedbittestandset((volatile long*)&m->lock, 0);
}

void ebi_mutex_unlock(ebi_mutex *m)
{
	_InterlockedExchange((volatile long*)&m->lock, 0);
	if (m->waiters > 0) {
		WakeByAddressSingle(&m->lock);
	}
}

// Event

// TODO: Implement this internally

typedef struct ebi_fence {
	uint32_t lock;
} ebi_fence;

void ebi_fence_close(ebi_fence *e)
{
	_InterlockedExchange((volatile long*)&e->lock, 1);
}

void ebi_fence_open(ebi_fence *e)
{
	_InterlockedExchange((volatile long*)&e->lock, 0);
	WakeByAddressAll(&e->lock);
}

void ebi_fence_wait(ebi_fence *e)
{
	uint32_t cmp = 0;
	uint32_t spin = 0;
	for (;;) {
		if (_InterlockedExchangeAdd((volatile long*)&e->lock, 0) == 0) break;
		if (++spin > 1000) {
			WaitOnAddress(&e->lock, &cmp, 4, INFINITE);
		}
	}
}

// Utility

static ebi_forceinline size_t ebi_grow_sz(size_t size, size_t min)
{
	return size > min ? size * 2 : min;
}

// -- Core

#define EBI_OBJLIST_SIZE 64
#define EBI_MAX_DEFER_LINKS 64

typedef struct ebi_obj ebi_obj;
typedef struct ebi_gc_gen ebi_gc_gen;
typedef struct ebi_pool ebi_pool;
typedef struct ebi_objlist ebi_objlist;
typedef struct ebi_objlink ebi_objlink;

// Shared allocation for small similarly-sized objects
struct ebi_pool {

	// Pointer to the next entry in an intrusive atomic `ebi_ia_list`
	ebi_pool *next;

	// Bit-mask of allocated slots in the pool
	uint32_t alloc_mask[4];
};

// Generation counters for garbage collection. We mark objects with a number
// instead of a mark bit so instead of clearing the marks we can increase the
// reference value `vm->gen`. In addition we have two sets: G and N. Objects in
// G are only traversed/collected during major collcetions while N are always.
//
// Pointers from G to N are not allowed at the end of the mark phase, this is
// enfored by promoting N objects (recursively) to G when pointed to from G.
//
// We use two separate byte-sized variables so we can assign to them without
// atomic operations. This is safe since even though we _do_ have race
// conditions, all threads try to write the same values: `vm->gen.g/n`.
// If `gen.g != 0` then `gen.n` is ignored!
struct ebi_gc_gen {
	uint8_t g, n;
};

// Heap object header
struct ebi_obj {
	ebi_type *type;
	uint32_t weak_slot;
	uint16_t pool_offset;
	ebi_gc_gen gen;
	char data[];
};

// List of objects that can be sent between threads
struct ebi_objlist {

	// Intrusive atomic `ebi_ia_list` link
	ebi_objlist *next;

	ebi_obj *objs[EBI_OBJLIST_SIZE];
	uint32_t count;
};

// Link between two heap objects, for example `src.prop = dst`.
struct ebi_objlink {
	void *src, *dst;
};

typedef enum ebi_alive_group {
	EBI_ALIVE_G,  // G objects, swept on major GC
	EBI_ALIVE_N1, // old N objects, swept on minor GC
	EBI_ALIVE_N2, // new N objects, not swept

	EBI_NUM_ALIVE_GROUPS,
} ebi_alive_group;

typedef enum ebi_gc_stage {
	EBI_GC_IDLE,
	EBI_GC_MARK,
	EBI_GC_SWEEP,
} ebi_gc_stage;

struct ebi_thread {
	ebi_vm *vm;

	// Monotonically increased value, the thread will synchronize with others
	// at `ebi_checkpoint()` if `et->checkpoint != vm->checkpoint`.
	// If `checkpoint_fence` is set all threads will stop and wait.
	uint32_t checkpoint;

	// Current GC generation, a local copy of `vm->gen`.
	ebi_gc_gen gen;

	// Mutex used to take ownership of this thread. Used eg. for scanning
	// stacks of halted threads.
	ebi_mutex mutex;
	bool lock_by_gc;

	ebi_objlist *objs_mark; // List of marked objects to traverse
	ebi_objlist *objs_alive[EBI_NUM_ALIVE_GROUPS]; // Alive objects per group

	// Deferred batched object to object links to process.
	ebi_objlink defer_links[EBI_MAX_DEFER_LINKS];
	size_t num_defer_links;
};

struct ebi_vm {
	// Hot data
	uint32_t checkpoint;
	bool checkpoint_fence;
	ebi_gc_gen gen;
	uint8_t pad[9];

	// Object lists
	ebi_ia_stack objs_mark;
	ebi_ia_stack objs_sweep;
	ebi_ia_stack objs_sweep_next;
	ebi_ia_stack objs_alive[EBI_NUM_ALIVE_GROUPS];
	ebi_ia_stack objs_reuse;

	// Threads
	ebi_mutex thread_mutex;
	ebi_fence thread_fence;
	ebi_fence thread_sync_fence;
	ebi_thread **threads;
	size_t num_threads;
	size_t max_threads;

	// GC state
	ebi_mutex gc_mutex;
	ebi_gc_stage gc_stage;
	bool gc_major;
};


#if EBI_DEBUG
ebi_forceinline ebi_obj *ebi_get_obj(void *inst)
{
	ebi_assert(inst);
	ebi_obj *obj = (ebi_obj*)inst - 1;
	ebi_assert(obj->type);
	return obj;
}
#else
	#define ebi_get_obj(inst) ((ebi_obj*)(inst) - 1)
#endif

// Allocate a new empty object list.
ebi_objlist *ebi_alloc_objlist(ebi_vm *vm)
{
	ebi_objlist *list = ebi_ia_pop(&vm->objs_reuse);
	if (!list) {
		list = (ebi_objlist*)malloc(sizeof(ebi_objlist));
		ebi_assert(list);
		list->next = NULL;
	}
	list->count = 0;
	return list;
}

// Send the current to-mark list to GC threads to process
ebi_objlist *ebi_flush_marks(ebi_thread *et)
{
	ebi_vm *vm = et->vm;
	if (et->objs_mark->count > 0) {
		ebi_ia_push(&vm->objs_mark, et->objs_mark);
		et->objs_mark = ebi_alloc_objlist(vm);
	}
	return et->objs_mark;
}

ebi_objlist *ebi_flush_alive(ebi_thread *et, ebi_alive_group group)
{
	ebi_vm *vm = et->vm;
	if (et->objs_alive[group]->count > 0) {
		ebi_ia_push(&vm->objs_alive[group], et->objs_alive[group]);
		et->objs_alive[group] = ebi_alloc_objlist(vm);
	}
	return et->objs_alive[group];
}

// If the object type has references push it to the to-mark list.
ebi_forceinline void ebi_queue_mark(ebi_thread *et, ebi_obj *obj)
{
	if (obj->type->flags & EBI_TYPE_HAS_REFS) {
		ebi_objlist *list = et->objs_mark;
		if (list->count == EBI_OBJLIST_SIZE) {
			list = ebi_flush_marks(et);
		}
		list->objs[list->count++] = obj;
	}
}

// Mark `ptr`, promote the object to G if `to_g == true`.
ebi_forceinline void ebi_mark(ebi_thread *et, void *ptr, bool to_g)
{
	ebi_obj *obj = ebi_get_obj(ptr);

	// Update the active generation
	if (obj->gen.g | to_g) {
		if (obj->gen.g == et->gen.g) return;
		obj->gen.g = et->gen.g;
	} else {
		if (obj->gen.n == et->gen.n) return;
		obj->gen.n = et->gen.n;
	}
	ebi_queue_mark(et, obj);
}

ebi_forceinline void ebi_add_alive(ebi_thread *et, ebi_obj *obj, ebi_alive_group group)
{
	ebi_objlist *list = et->objs_alive[group];
	if (list->count == EBI_OBJLIST_SIZE) {
		list = ebi_flush_alive(et, group);
	}
	list->objs[list->count++] = obj;
}

void ebi_mark_fields(ebi_thread *et, void *ptr, ebi_type *type, bool to_g);

// Mark a complex object of `type` located at `ptr`.
ebi_forceinline void ebi_mark_type(ebi_thread *et, void *ptr, ebi_type *type, bool to_g)
{
	uint32_t flags = type->flags;
	if (flags & EBI_TYPE_IS_REF) {
		void *value = *(void**)ptr;
		if (value) {
			ebi_mark(et, value, to_g);
		}
	} else if (flags & EBI_TYPE_HAS_REFS) {
		ebi_mark_fields(et, ptr, type, to_g);
	}
}

// Mark fields of a `type` at `ptr`.
void ebi_mark_fields(ebi_thread *et, void *ptr, ebi_type *type, bool to_g)
{
	ebi_assert(type->flags & EBI_TYPE_HAS_REFS);

	char *inst_ptr = (char*)ptr;
	size_t num_fields = type->num_fields;

	ebi_field *begin = type->fields, *end = begin + num_fields;
	for (ebi_field *f = begin; f != end; f++) {
		ebi_mark_type(et, inst_ptr + f->offset, f->type, to_g);
	}

	if (type->flags & EBI_TYPE_HAS_SUFFIX) {
		ebi_type *suf_type = end->type;
		size_t suf_stride = suf_type->data_size, suf_num = *(uint32_t*)inst_ptr;
		char *suf_ptr = inst_ptr + type->data_size;

		// Optimized suffix marking as arrays tend to be larger than structs
		// TODO: Optimization for `EBI_TYPE_HAS_ONE_REF` ?
		if (suf_type->flags & EBI_TYPE_IS_REF) {
			while (suf_num > 0) {
				void *value = *(void**)suf_ptr;
				if (value) {
					ebi_mark(et, value, to_g);
				}
				suf_num--;
				suf_ptr += suf_stride;
			}
		} else if (suf_type->flags & EBI_TYPE_HAS_REFS) {
			while (suf_num > 0) {
				ebi_mark_fields(et, suf_ptr, suf_type, to_g);
				suf_num--;
				suf_ptr += suf_stride;
			}
		}
	}
}

// Flush deferred object links
void ebi_flush_links(ebi_thread *et)
{
	size_t num = et->num_defer_links;
	if (!num) return;

	// TODO: Memory barrier here

	for (size_t i = 0; i < num; i++) {
		ebi_objlink link = et->defer_links[i];
		uint32_t src_g = ebi_get_obj(link.src)->gen.g;
		uint32_t dst_g = ebi_get_obj(link.dst)->gen.g;

		// Promote `dst` to G for `N->G` and `G->N` links. Note that this will
		// also "promote" `dst` if both are in G with different genrations.
		ebi_mark(et, link.dst, (src_g ^ dst_g) != 0);
	}
}

// Defer an object link mark
ebi_forceinline void ebi_defer_link(ebi_thread *et, void *src, void *dst)
{
	if (et->num_defer_links == EBI_MAX_DEFER_LINKS) {
		ebi_flush_links(et);
	}
	ebi_objlink *link = &et->defer_links[et->num_defer_links++];
	link->src = src;
	link->dst = dst;
}

// Assign reference at `inst + offset` to `value`. Issue write barriers to
// not hide references from the GC threads.
void ebi_assign_ref(ebi_thread *et, void *inst, size_t offset, void *value)
{
	void **slot = (void**)((char*)inst + offset);

	// Always issue an Yuasa deletion barrier for the previous value
	void *prev = *slot;
	if (prev) {
		ebi_mark(et, prev, false);
	}

	// Defer other barriers to reduce the amount of memory fences
	ebi_defer_link(et, inst, value);

	*slot = (void*)value;
}

// Advance the mark phase of GC.
// Returns `true` if there was something to mark.
bool ebi_gc_mark(ebi_thread *et)
{
	ebi_vm *vm = et->vm;
	ebi_objlist *list = ebi_ia_pop(&vm->objs_mark);
	if (!list) return false;

	// Objects only end up in this list if `EBI_TYPE_HAS_REFS` so we can safely
	// call `ebi_mark_fields()` directly wihtout a check.
	uint32_t count = list->count;
	for (uint32_t oi = 0; oi < count; oi++) {
		ebi_obj *obj = list->objs[oi];
		ebi_mark_fields(et, obj->data, obj->type, obj->gen.g != 0);
	}

	ebi_ia_push(&vm->objs_reuse, list);
	return true;
}

ebi_forceinline bool ebi_alive(ebi_gc_gen cur, ebi_gc_gen gen)
{
	uint32_t dg = ((uint32_t)gen.g - (uint32_t)cur.g) & 0xff;
	uint32_t dn = ((uint32_t)gen.n - (uint32_t)cur.n) & 0xff;
	return (dg < 128) | ((gen.g == 0) & (dn < 128));
}

void ebi_free_obj(ebi_thread *et, ebi_obj *obj)
{
#if 0
	if (obj->weak_slot) {
		// TODO: Batch these?
		ebi_mutex_lock(&vm->weak_mutex);
		ebi_weak_slot *slot = &vm->weak_slots[obj->weak_slot];
		if (slot->gen != UINT32_MAX) {
			slot->val.next_free = vm->weak_free_head;
			vm->weak_free_head = obj->weak_slot;
			slot->gen++;
		}
		ebi_mutex_unlock(&vm->weak_mutex);
	}
#endif

	free(obj);
}

// Advance the sweep phase of GC.
// Returns `true` if there was something to sweep.
bool ebi_gc_sweep(ebi_thread *et)
{
	ebi_vm *vm = et->vm;
	ebi_objlist *list = ebi_ia_pop(&vm->objs_sweep);
	if (!list) {
		if (ebi_ia_maybe_nonempty(&vm->objs_sweep_next)) {
			list = ebi_ia_pop_all(&vm->objs_sweep_next);
			if (list) {
				ebi_ia_push_all(&vm->objs_sweep, list);
				if (list->next) {
					ebi_ia_push_all(&vm->objs_sweep_next, list->next);
				}
			}
		}
		if (!list) return false;
	}

	ebi_gc_gen gen = et->gen;

	uint32_t count = list->count;
	for (uint32_t oi = 0; oi < count; oi++) {
		ebi_obj *obj = list->objs[oi];
		if (ebi_alive(gen, obj->gen)) {
			ebi_add_alive(et, obj, obj->gen.g ? EBI_ALIVE_G : EBI_ALIVE_N1);
		} else {
			ebi_free_obj(et, obj);
		}
	}

	ebi_ia_push(&vm->objs_reuse, list);
	return true;
}

ebi_obj *ebi_alloc_obj(ebi_thread *et, ebi_type *type, size_t size)
{
	ebi_obj *obj = (ebi_obj*)malloc(sizeof(ebi_obj) + size);
	if (!obj) return NULL;

	obj->type = type;
	obj->weak_slot = 0;
	obj->pool_offset = 0;
	obj->gen.g = 0;
	obj->gen.n = et->gen.n;
	ebi_add_alive(et, obj, EBI_ALIVE_N2);

	return obj;
}

void *ebi_new(ebi_thread *et, ebi_type *type)
{
	size_t size = type->data_size;
	ebi_obj *obj = ebi_alloc_obj(et, type, size);
	if (!obj) return NULL;

	void *data = obj + 1;
	memset(data, 0, size);
	return data;
}

void ebi_synchronize_thread(ebi_thread *et, bool wait_marks)
{
	ebi_vm *vm = et->vm;
	if (et->checkpoint == vm->checkpoint) return;

	for (uint32_t i = 0; i < EBI_NUM_ALIVE_GROUPS; i++) {
		ebi_flush_alive(et, (ebi_alive_group)i);
	}

	do {
		ebi_flush_links(et);
		ebi_flush_marks(et);
	} while (wait_marks && ebi_gc_mark(et));

	et->gen = vm->gen;
	et->checkpoint = vm->checkpoint;
}

void ebi_synchronize_thread_fence(ebi_thread *et)
{
	ebi_vm *vm = et->vm;

	if (vm->checkpoint_fence) {
		ebi_synchronize_thread(et, true);
		ebi_mutex_unlock(&et->mutex);
		ebi_fence_wait(&vm->thread_fence);
		ebi_mutex_lock(&et->mutex);
	} else {
		ebi_synchronize_thread(et, false);
	}
}

void ebi_checkpoint(ebi_thread *et)
{
	ebi_vm *vm = et->vm;
	if (et->checkpoint != vm->checkpoint) {
		ebi_synchronize_thread_fence(et);
	}
}


void ebi_gc_thread_barrier(ebi_thread *et)
{
	ebi_vm *vm = et->vm;

	ebi_mutex_lock(&vm->thread_mutex);
	ebi_fence_close(&vm->thread_fence);

	vm->checkpoint_fence = true;
	vm->gen.n = vm->gen.n == 255 ? 1 : vm->gen.n + 1;

	// TODO: Atomic release
	vm->checkpoint++;

	for (uint32_t i = 0; i < vm->num_threads; i++) {
		ebi_thread *ot = vm->threads[i];
		ebi_mutex_lock(&ot->mutex);
		ebi_synchronize_thread(ot, false);
	}

	for (uint32_t i = 0; i < vm->num_threads; i++) {
		ebi_thread *ot = vm->threads[i];
		ebi_mutex_unlock(&ot->mutex);
	}

	ebi_fence_open(&vm->thread_fence);
	ebi_mutex_unlock(&vm->thread_mutex);

	return true;
}

void ebi_mark_globals(ebi_thread *et, bool to_g)
{
	ebi_vm *vm = et->vm;

}


void ebi_gc_step(ebi_thread *et)
{
	ebi_vm *vm = et->vm;

	ebi_checkpoint(et);

	uintptr_t mark_count = ebi_ia_get_count(&vm->objs_mark);
	bool mark = ebi_gc_mark(et);
	bool sweep = ebi_gc_sweep(et);
	if (!mark && et->objs_mark->count) {
		ebi_flush_marks(et);
		mark = ebi_gc_mark(et);
	}

	ebi_mutex_lock(&vm->gc_mutex);
	switch (vm->gc_stage) {
	case EBI_GC_IDLE:
		ebi_mark_globals(et, vm->gc_major);
		vm->gc_stage = EBI_GC_MARK;
		break;
	case EBI_GC_MARK:
		if (!mark) {
			ebi_thread_barrier(et);
			vm->gc_stage = EBI_GC_SWEEP;
			ebi_ia_push_all(&vm->objs_sweep, ebi_ia_pop_all(&vm->objs_alive[EBI_ALIVE_N1]));
			if (vm->gc_major) {
				ebi_ia_push_all(&vm->objs_sweep_next, ebi_ia_pop_all(&vm->objs_alive[EBI_ALIVE_G]));
			}
		}
		break;
	case EBI_GC_SWEEP:
		vm->gc_stage = EBI_GC_IDLE;
		break;
	}
	ebi_mutex_unlock(&vm->gc_mutex);
}

#if 0

// Object list

#define EBI_OBJLIST_SIZE 64
#define EBI_MARKLIST_SIZE 64

typedef struct ebi_objlist ebi_objlist;
struct ebi_objlist {
	ebi_objlist *next;
	ebi_obj *objs[EBI_OBJLIST_SIZE];
	uint32_t count;
};

typedef struct ebi_deferred_mark ebi_deferred_mark;

struct ebi_obj {
	ebi_type *type;
	uint32_t weak_slot;
	uint16_t block_offset;
	uint8_t epoch_g;
	uint8_t epoch_n;
};

struct ebi_deferred_mark {
	void *src;
	void *dst;
};

struct ebi_thread {
	ebi_vm *vm;
	uint32_t checkpoint;

	uint8_t epoch_g;
	uint8_t epoch_n;
	ebi_mutex mutex;

	ebi_objlist *objs_mark;
	ebi_objlist *objs_alive;

	ebi_deferred_mark deferred_marks[EBI_MARKLIST_SIZE];
	size_t num_deferred_marks;
};

typedef enum {
	EBI_GC_MARK,
	EBI_GC_MARK_SYNC,
	EBI_GC_SWEEP,
	EBI_GC_SWEEP_SYNC,
} ebi_gc_stage;

typedef struct {
	union {
		ebi_obj *obj;
		uint32_t next_free;
	} val;
	uint32_t gen;
} ebi_weak_slot;

typedef struct {
	uint32_t hash;
	uint32_t weak_ix;
	uint32_t weak_gen;
} ebi_intern_slot;

struct ebi_vm {
	// Hot data
	uint32_t checkpoint;
	uint8_t epoch_g;
	uint8_t epoch_n;
	uint8_t pad[10];

	// Object lists
	ebi_ia_stack objs_mark;
	ebi_ia_stack objs_sweep;
	ebi_ia_stack objs_alive;
	ebi_ia_stack objs_reuse;

	// Threads
	ebi_mutex thread_mutex;
	ebi_thread **threads;
	size_t num_threads;
	size_t max_threads;

	// GC state
	ebi_mutex gc_mutex;
	ebi_gc_stage gc_stage;

	// Misc
	ebi_thread *main_thread;
	ebi_types types;

	// Weak references
	ebi_mutex weak_mutex;
	ebi_weak_slot *weak_slots;
	size_t num_weak_slots;
	size_t max_weak_slots;
	uint32_t weak_free_head;

	// Intern table (uses weak_mutex)
	ebi_intern_slot *intern_slots;
	size_t num_intern_slots;
	size_t max_intern_slots;
	size_t cap_intern_slots;
};

ebi_objlist *ebi_alloc_objlist(ebi_vm *vm)
{
	ebi_objlist *list = ebi_ia_pop(&vm->objs_reuse);
	if (!list) {
		list = (ebi_objlist*)malloc(sizeof(ebi_objlist));
		list->next = NULL;
	}
	list->count = 0;
	return list;
}

static void ebi_apply_marks(ebi_thread *et)
{
	size_t num = et->num_deferred_marks;
	et->num_deferred_marks = 0;

	// TODO: Barrier here

	for (size_t i = 0; i < num; i++) {
		ebi_deferred_mark *mark = &et->deferred_marks[i];
		ebi_obj *src = (ebi_obj*)mark->src - 1, *dst = (ebi_obj*)mark->dst - 1;
		if (src->epoch_g ^ dst->epoch_g) {
			ebi_mark_g(et, dst + 1);
		} else {
			ebi_mark_n(et, dst + 1);
		}
	}
}

ebi_objlist *ebi_flush_marks(ebi_thread *et)
{
	ebi_vm *vm = et->vm;
	if (et->objs_mark->count > 0) {
		ebi_ia_push(&vm->objs_mark, et->objs_mark);
		et->objs_mark = ebi_alloc_objlist(vm);
	}
	return et->objs_mark;
}

ebi_objlist *ebi_flush_alive(ebi_thread *et)
{
	ebi_vm *vm = et->vm;
	if (et->objs_alive->count > 0) {
		ebi_ia_push(&vm->objs_alive, et->objs_alive);
		et->objs_alive = ebi_alloc_objlist(vm);
	}
	return et->objs_alive;
}

bool ebi_thread_barrier(ebi_thread *et)
{
	ebi_vm *vm = et->vm;
	ebi_checkpoint(et);

	ebi_mutex_lock(&vm->thread_mutex);
	for (uint32_t i = 0; i < vm->num_threads; i++) {
		ebi_thread *ot = vm->threads[i];
		if (ot->checkpoint == vm->checkpoint) continue;
		if (ebi_mutex_try_lock(&ot->mutex)) {
			ebi_checkpoint(ot);
			ebi_mutex_unlock(&ot->mutex);
		} else {
			ebi_mutex_unlock(&vm->thread_mutex);
			return false;
		}
	}
	ebi_mutex_unlock(&vm->thread_mutex);
	return true;
}

ebi_forceinline void ebi_mark_g(ebi_thread *et, void *ptr)
{
	ebi_obj *obj = (ebi_obj*)ptr - 1;
	if (obj->epoch_g == et->epoch_g) return;
	obj->epoch_g = et->epoch_g;

	if (obj->type->flags & EBI_TYPE_HAS_REFS) {
		ebi_objlist *list = et->objs_mark;
		if (list->count == EBI_OBJLIST_SIZE) {
			list = ebi_flush_marks(et);
		}
		list->objs[list->count++] = obj;
	}
}

ebi_forceinline void ebi_mark_n(ebi_thread *et, void *ptr)
{
	ebi_obj *obj = (ebi_obj*)ptr - 1;
	if (obj->epoch_g) {
		if (obj->epoch_g == et->epoch_g) return;
		obj->epoch_g = et->epoch_g;
	} else {
		if (obj->epoch_n == et->epoch_n) return;
		obj->epoch_n = et->epoch_n;
	}

	if (obj->type->flags & EBI_TYPE_HAS_REFS) {
		ebi_objlist *list = et->objs_mark;
		if (list->count == EBI_OBJLIST_SIZE) {
			list = ebi_flush_marks(et);
		}
		list->objs[list->count++] = obj;
	}
}

ebi_forceinline void ebi_add_obj(ebi_thread *et, ebi_obj *obj)
{
	ebi_objlist *list = et->objs_alive;
	if (list->count == EBI_OBJLIST_SIZE) {
		list = ebi_flush_alive(et);
	}
	list->objs[list->count++] = obj;
}

void ebi_mark_slots(ebi_thread *et, void *ptr, ebi_type *type)
{
	ebi_mark(et, type);
	if ((type->flags & EBI_TYPE_HAS_REFS) == 0) return;

	char *inst_ptr = (char*)ptr;
	size_t num_fields = type->num_fields;
	ebi_field *begin = type->fields, *end = begin + num_fields;
	for (ebi_field *f = begin; f != end; f++) {
		if (f->flags & EBI_FIELD_IS_REF) {
			ebi_mark(et, *(void**)(inst_ptr + f->offset));
		} else if (f->flags & EBI_TYPE_HAS_REFS) {
			ebi_mark_slots(et, inst_ptr + f->offset, f->type);
		}
	}
}

void ebi_mark_slots_many(ebi_thread *et, void *ptr, ebi_type *type, size_t count)
{
	ebi_mark(et, type);
	if ((type->flags & EBI_TYPE_HAS_REFS) == 0) return;

	char *inst_ptr = (char*)ptr;

	size_t num_fields = type->num_fields;
	size_t stride = type->data_size;
	ebi_field *begin = type->fields, *end = begin + num_fields;
	for (; count > 0; count--) {
		for (ebi_field *f = begin; f != end; f++) {
			if (f->flags & EBI_FIELD_IS_REF) {
				ebi_mark(et, *(void**)(inst_ptr + f->offset));
			} else if (f->flags & EBI_TYPE_HAS_REFS) {
				ebi_mark_slots(et, inst_ptr + f->offset, f->type);
			}
		}
		inst_ptr += stride;
	}
}

void ebi_mark_slots_heap(ebi_thread *et, void *ptr, ebi_type *type)
{
	ebi_mark(et, type);
	if ((type->flags & EBI_TYPE_HAS_REFS) == 0) return;

	char *inst_ptr = (char*)ptr;
	size_t num_fields = type->num_fields;
	ebi_field *begin = type->fields, *end = begin + num_fields;

	if (type->flags & EBI_TYPE_HAS_SUFFIX) {
		ebi_field *suffix = --end;
		ebi_mark_slots_many(et, inst_ptr + type->data_size,
			suffix->type, *(size_t*)inst_ptr);
	}

	for (ebi_field *f = begin; f != end; f++) {
		if (f->flags & EBI_FIELD_IS_REF) {
			ebi_mark(et, *(void**)(inst_ptr + f->offset));
		} else if (f->flags & EBI_TYPE_HAS_REFS) {
			ebi_mark_slots(et, inst_ptr + f->offset, f->type);
		}
	}
}

void ebi_mark_globals(ebi_thread *et)
{
	ebi_vm *vm = et->vm;

	// Types
	size_t num_types = sizeof(ebi_types) / sizeof(ebi_type*);
	ebi_mark_slots_many(et, &vm->types, vm->types.object, num_types);
}

void *ebi_alloc_heavy(size_t size)
{
	return _aligned_malloc((size + 63) & ~(size_t)63, 64);
}

void ebi_free_heavy(void *ptr)
{
	_aligned_free(ptr);
}

ebi_weak_ref ebi_make_weak_ref_no_mutex(ebi_thread *et, ebi_ptr void *ptr)
{
	ebi_vm *vm = et->vm;
	ebi_obj *obj = (ebi_obj*)ptr - 1;
	ebi_weak_ref ref = 0;

	uint32_t gen, slot_ix = obj->weak_slot;
	if (slot_ix == 0) {
		slot_ix = vm->weak_free_head;
		if (slot_ix != 0) {
			vm->weak_free_head = vm->weak_slots[slot_ix].val.next_free;
		} else {
			if (vm->num_weak_slots >= vm->max_weak_slots) {
				uint32_t prev_max = 0;
				vm->max_weak_slots = ebi_grow_sz(vm->max_weak_slots, 64);
				vm->weak_slots = realloc(vm->weak_slots, vm->max_weak_slots * sizeof(ebi_weak_slot));
				memset(vm->weak_slots + prev_max, 0,
					(vm->max_weak_slots - prev_max) * sizeof(ebi_weak_slot));
			}
			slot_ix = (uint32_t)vm->num_weak_slots++;
		}
		ebi_weak_slot *slot = &vm->weak_slots[slot_ix];
		slot->val.obj = obj;
		gen = ++slot->gen;
		obj->weak_slot = slot_ix;

	} else {
		gen = vm->weak_slots[slot_ix].gen;
	}

	ref = (uint64_t)slot_ix << 32 | (uint64_t)gen;

	return ref;
}

ebi_ptr void *ebi_resolve_weak_ref_no_mutex(ebi_thread *et, ebi_weak_ref ref)
{
	ebi_vm *vm = et->vm;

	void *ptr = NULL;
	uint32_t gen = (uint32_t)ref, slot_ix = (uint32_t)(ref >> 32);

	ebi_weak_slot *slot = &vm->weak_slots[slot_ix];
	if (slot->gen == gen) {
		ebi_obj *obj = slot->val.obj;
		ptr = obj + 1;
		if (obj->epoch_g != et->epoch_g) {
			// If this object hasn't been marked by the GC in this cycle
			// we might be in trouble: If we're sweeping it's too late
			// to revive this reference as it will be deleted, otherwise
			// mark the object to make sure it doesn't get deleted.
			ebi_mutex_lock(&vm->gc_mutex);
			if (vm->gc_stage == EBI_GC_SWEEP) {
				ptr = NULL;
			} else {
				ebi_mark(et, obj + 1);
			}
			ebi_mutex_unlock(&vm->gc_mutex);
		}
	}

	return ptr;
}

bool ebi_is_weak_probably_valid_no_mutex(ebi_thread *et, uint32_t slot_ix, uint32_t gen)
{
	ebi_vm *vm = et->vm;

	ebi_weak_slot *slot = &vm->weak_slots[slot_ix];
	if (slot->gen == gen) {
		ebi_obj *obj = slot->val.obj;
		if (obj->epoch != et->epoch) {
			return vm->gc_stage != EBI_GC_SWEEP;
		} else {
			return true;
		}
	}

	return false;
}

uint32_t ebi_intern_hash(const char *data, size_t length)
{
	uint32_t hash = 0x811c9dc5;
	for (size_t i = 0; i < length; i++) {
		hash = (hash ^ (uint8_t)data[i]) * 0x01000193;
	}
	return hash;
}

void ebi_intern_rehash(ebi_thread *et)
{
	ebi_vm *vm = et->vm;
	ebi_intern_slot *slots = vm->intern_slots;

	// Try to remove expired references first
	uint32_t max_slots = (uint32_t)vm->max_intern_slots;
	uint32_t num_slots = (uint32_t)vm->num_intern_slots;
	uint32_t mask = max_slots - 1;
	for (uint32_t i = 0; i < max_slots; i++) {
		ebi_intern_slot *s = &slots[i];
		if (!s->weak_ix) continue;
		if (ebi_is_weak_probably_valid_no_mutex(et, s->weak_ix, s->weak_gen)) continue;

		num_slots--;

		// Shift back deletion
		uint32_t next_i = (i + 1) & mask;
		for (;;) {
			ebi_intern_slot *sn = &slots[next_i];
			if (!sn->weak_ix || (sn->hash & mask) == next_i) break;
			*s = *sn;
			s = sn;
			next_i = (i + 1) & mask;
		}
		s->hash = 0;
		s->weak_ix = 0;
		s->weak_gen = 0;
	}

	// If we didn't manage to delete at least half of
	// the entries re-hash into a larger map.
	if (num_slots >= vm->cap_intern_slots / 2) {
		vm->max_intern_slots = ebi_grow_sz(max_slots, 64);
		size_t sz = vm->max_intern_slots * sizeof(ebi_intern_slot);
		ebi_intern_slot *new_slots = malloc(sz);
		memset(new_slots, 0, sz);

		uint32_t new_mask = (uint32_t)vm->max_intern_slots - 1;

		for (uint32_t old_i = 0; old_i < max_slots; old_i++) {
			ebi_intern_slot *s = &slots[old_i];
			if (!s->weak_ix) continue;

			ebi_intern_slot slot = *s;
			uint32_t ix = slot.hash & new_mask;
			uint32_t scan = 0;
			for (;;) {
				ebi_intern_slot *ns = &slots[ix];
				uint32_t nscan = (ix - ns->hash) & new_mask;
				if (!ns->weak_ix) {
					*ns = slot;
					break;
				} else if (nscan < scan) {
					*ns = slot;
					slot = *ns;
					scan = nscan;
				}
				scan++;
				ix = (ix + 1) & mask;
			}
		}

		// Use maximum capacity of 7/8
		free(vm->intern_slots);
		vm->intern_slots = new_slots;
		vm->cap_intern_slots = vm->max_intern_slots - vm->max_intern_slots / 8;
	}
}

// API

ebi_vm *ebi_make_vm()
{
	ebi_vm *vm = ebi_alloc_heavy(sizeof(ebi_vm));
	if (!vm) return NULL;
	memset(vm, 0, sizeof(ebi_vm));

	ebi_thread *et = ebi_make_thread(vm);
	vm->main_thread = et;

	ebi_lock_thread(et);

	ebi_type temp_info_type;
	temp_info_type.data_size = sizeof(ebi_type_info);
	temp_info_type.elem_size = sizeof(ebi_field_info);
	ebi_type_info *type_info = ebi_new_array(et, &temp_info_type, 3);


#if 0
	// Bootstrap the type of type
	ebi_type temp_type_type = { 0 };
	temp_type_type.size = sizeof(ebi_type);
	vm->types.type = ebi_new(et, &temp_type_type, 1);
	ebi_obj *types_obj = (ebi_obj*)vm->types.type - 1;
	types_obj->type = vm->types.type;
	vm->types.type->size = sizeof(ebi_type);

	size_t num_types = sizeof(ebi_types) / sizeof(ebi_type*);
	ebi_type **p_types = (ebi_type**)&vm->types;
	for (size_t i = 1; i < num_types; i++) {
		p_types[i] = ebi_new(et, vm->types.type, 1);
	}

	// Reserve zero as NULL
	vm->num_weak_slots = 1;

	vm->types.char_->size = sizeof(char);
	vm->types.u32->size = sizeof(uint32_t);

	{
		ebi_type *t = vm->types.type;
		const uint32_t slots[] = {
			offsetof(ebi_type, slots),
			offsetof(ebi_type, name.data),
		};
		t->name = ebi_new_stringz(et, "ebi.Type");
		t->size = sizeof(ebi_type);
		t->slots = ebi_new_copy(et, vm->types.u32, ebi_arraycount(slots), slots);
	}

	{
		ebi_type *t = vm->types.char_;
		t->name = ebi_new_stringz(et, "Char");
		t->size = sizeof(char);
		t->slots = NULL;
	}

	{
		ebi_type *t = vm->types.u32;
		t->name = ebi_new_stringz(et, "U32");
		t->size = sizeof(uint32_t);
		t->slots = NULL;
	}

	{
		ebi_type *t = vm->types.string;
		const uint32_t slots[] = { offsetof(ebi_string, data), };
		t->name = ebi_new_stringz(et, "String");
		t->size = sizeof(ebi_string);
		t->slots = ebi_new_copy(et, vm->types.u32, ebi_arraycount(slots), slots);
	}

	{
		ebi_type *t = vm->types.object;
		const uint32_t slots[] = { 0 };
		t->name = ebi_new_stringz(et, "Object");
		t->size = sizeof(void*);
		t->slots = ebi_new_copy(et, vm->types.u32, ebi_arraycount(slots), slots);
	}

	{
		ebi_type *t = vm->types.type_desc;
		const uint32_t slots[] = {
			offsetof(ebi_type_desc, slots),
			offsetof(ebi_type_desc, name.data),
		};
		t->name = ebi_new_stringz(et, "ebi.TypeDesc");
		t->size = sizeof(ebi_type_desc);
		t->slots = ebi_new_copy(et, vm->types.u32, ebi_arraycount(slots), slots);
	}

#endif

	ebi_unlock_thread(et);

	return vm;
}

ebi_thread *ebi_make_thread(ebi_vm *vm)
{
	ebi_thread *et = ebi_alloc_heavy(sizeof(ebi_thread));
	if (!et) return NULL;
	memset(et, 0, sizeof(ebi_thread));

	et->vm = vm;
	et->epoch = vm->epoch;
	et->checkpoint = vm->checkpoint;

	et->objs_mark = ebi_alloc_objlist(vm);
	et->objs_alive = ebi_alloc_objlist(vm);

	// TEMP
	et->stack.base = malloc(16 * 1024);
	et->stack.frames = (ebi_callframe*)malloc(sizeof(ebi_callframe) * 128);
	et->stack.top = et->stack.base;

	ebi_mutex_lock(&vm->thread_mutex);
	if (vm->num_threads == vm->max_threads) {
		vm->max_threads = ebi_grow_sz(vm->max_threads, 16);
		vm->threads = realloc(vm->threads, vm->max_threads);
	}
	vm->threads[vm->num_threads++] = et;
	ebi_mutex_unlock(&vm->thread_mutex);

	return et;
}

ebi_types *ebi_get_types(ebi_vm *vm)
{
	return &vm->types;
}

ebi_forceinline static void ebi_init_obj(ebi_thread *et, ebi_obj *obj, ebi_type *type)
{
	obj->epoch_n = et->epoch_n;
	obj->epoch_g = 0;
	obj->weak_slot = 0;
	obj->type = type;
	ebi_add_obj(et, obj);
}

void *ebi_new(ebi_thread *et, ebi_type *type)
{
	size_t size = type->data_size;
	ebi_obj *obj = (ebi_obj*)malloc(sizeof(ebi_obj) + size);
	if (!obj) return NULL;
	ebi_init_obj(et, obj, type);

	void *data = obj + 1;
	memset(data, 0, size);
	return data;
}

void *ebi_new_uninit(ebi_thread *et, ebi_type *type)
{
	size_t size = type->data_size;
	ebi_obj *obj = (ebi_obj*)malloc(sizeof(ebi_obj) + size);
	if (!obj) return NULL;
	ebi_init_obj(et, obj, type);

	void *data = obj + 1;
	return data;
}

void *ebi_new_array(ebi_thread *et, ebi_type *type, size_t count)
{
	ebi_assert(type->elem_size);
	size_t size = type->data_size + type->elem_size * count;
	ebi_obj *obj = (ebi_obj*)malloc(sizeof(ebi_obj) + size);
	if (!obj) return NULL;
	ebi_init_obj(et, obj, type);

	void *data = obj + 1;
	memset(data, 0, size);
	*(size_t*)data = count;
	return data;
}

void *ebi_new_array_uninit(ebi_thread *et, ebi_type *type, size_t count)
{
	ebi_assert(type->elem_size);
	size_t size = type->data_size + type->elem_size * count;
	ebi_obj *obj = (ebi_obj*)malloc(sizeof(ebi_obj) + size);
	if (!obj) return NULL;
	ebi_init_obj(et, obj, type);

	void *data = obj + 1;
	*(size_t*)data = count;
	return data;
}

void ebi_set(ebi_thread *et, void *inst, size_t offset, const void *value)
{
	void **slot = (void**)(char*)inst + offset;
	void *prev = *slot;
	if (prev) ebi_mark_n(et, *slot);

	if (value) {
		if (et->num_deferred_marks == EBI_MARKLIST_SIZE) {
			ebi_apply_marks(et);
		}

		ebi_deferred_mark *mark = &et->deferred_marks[et->num_deferred_marks++];
		mark->src = inst;
		mark->dst = value;
	}

	*slot = (void*)value;
}

void ebi_set_string(ebi_thread *et, ebi_string *dst, const ebi_string *src)
{
	ebi_set(et, &dst->data, (void*)src->data);
	dst->begin = src->begin;
	dst->length = src->length;
}

ebi_type *ebi_new_type(ebi_thread *et, const ebi_type_desc *desc)
{
#if 0
	ebi_type *type = ebi_new(et, et->vm->types.type);
	type->size = desc->size;
	ebi_set_string(et, &type->name, &desc->name);
	ebi_set(et, &type->slots, desc->slots);
	return type;
#endif
}

ebi_string ebi_new_string(ebi_thread *et, const char *data, size_t length)
{
	void *copy = (char*)ebi_new_uninit(et, et->vm->types.char_, length);
	memcpy(copy, data, length);
	ebi_string str = { copy, 0, length };
	return str;
}

ebi_string ebi_new_stringz(ebi_thread *et, const char *data)
{
	return ebi_new_string(et, data, strlen(data));
}

void ebi_lock_thread(ebi_thread *et)
{
	ebi_mutex_lock(&et->mutex);
	ebi_checkpoint(et);
}

void ebi_unlock_thread(ebi_thread *et)
{
	ebi_checkpoint(et);
	ebi_mutex_unlock(&et->mutex);
}

void ebi_synchronize_thread(ebi_thread *et)
{
	ebi_vm *vm = et->vm;
	if (et->epoch_g != vm->epoch_g) {
		et->epoch_g = vm->epoch_g;
		// TODO
	}
	if (et->epoch_n != vm->epoch_n) {
		et->epoch_n = vm->epoch_n;
		// TODO
	}
	ebi_apply_marks(et);
	ebi_flush_marks(et);
	ebi_flush_alive(et);
	et->checkpoint = vm->checkpoint;
}

void ebi_checkpoint(ebi_thread *et)
{
	ebi_vm *vm = et->vm;
	if (et->checkpoint != vm->checkpoint) {
		ebi_synchronize_thread(et);
	}
}

bool ebi_gc_mark(ebi_thread *et)
{
	ebi_vm *vm = et->vm;
	ebi_objlist *list = ebi_ia_pop(&vm->objs_mark);
	if (!list) return false;

	uint32_t count = list->count;
	for (uint32_t oi = 0; oi < count; oi++) {
		ebi_obj *obj = list->objs[oi];
		ebi_mark_slots(et, obj + 1, obj->type, obj->count);
	}

	ebi_ia_push(&vm->objs_reuse, list);
	return true;
}

bool ebi_gc_sweep(ebi_thread *et)
{
	ebi_vm *vm = et->vm;
	ebi_objlist *list = ebi_ia_pop(&vm->objs_sweep);
	if (!list) return false;

	uint8_t bad_epoch = (uint8_t)(vm->epoch - 1);
	uint32_t count = list->count;
	for (uint32_t oi = 0; oi < count; oi++) {
		ebi_obj *obj = list->objs[oi];
		if (obj->epoch != bad_epoch) {
			ebi_add_obj(et, obj);
		} else {
			if (obj->weak_slot) {
				// TODO: Batch these?
				ebi_mutex_lock(&vm->weak_mutex);
				ebi_weak_slot *slot = &vm->weak_slots[obj->weak_slot];
				if (slot->gen != UINT32_MAX) {
					slot->val.next_free = vm->weak_free_head;
					vm->weak_free_head = obj->weak_slot;
					slot->gen++;
				}
				ebi_mutex_unlock(&vm->weak_mutex);
			}

			free(obj);
		}
	}

	ebi_ia_push(&vm->objs_reuse, list);
	return true;
}

void ebi_gc_assist(ebi_thread *et)
{
	ebi_checkpoint(et);
	if (!ebi_gc_mark(et)) {
		ebi_flush_marks(et);
		if (!ebi_gc_mark(et)) {
			ebi_gc_sweep(et);
		}
	}
}

void ebi_gc_step(ebi_thread *et)
{
	ebi_vm *vm = et->vm;

	ebi_checkpoint(et);

	uintptr_t mark_count = ebi_ia_get_count(&vm->objs_mark);
	bool mark = ebi_gc_mark(et);
	bool sweep = ebi_gc_sweep(et);
	if (!mark && et->objs_mark->count) {
		ebi_flush_marks(et);
		mark = ebi_gc_mark(et);
	}

	ebi_mutex_lock(&vm->gc_mutex);
	switch (vm->gc_stage) {
	case EBI_GC_MARK:
		if (!mark) {
			vm->checkpoint++;
			vm->gc_stage = EBI_GC_MARK_SYNC;
		}
		break;
	case EBI_GC_MARK_SYNC:
		if (mark) {
			vm->gc_stage = EBI_GC_MARK;
		} else if (ebi_thread_barrier(et) && ebi_ia_get_count(&vm->objs_mark) == mark_count) {
			vm->gc_stage = EBI_GC_SWEEP;
			ebi_ia_push_all(&vm->objs_sweep, ebi_ia_pop_all(&vm->objs_alive));
		}
		break;
	case EBI_GC_SWEEP:
		if (!sweep) {
			vm->checkpoint++;
			vm->epoch++;
			vm->gc_stage = EBI_GC_SWEEP_SYNC;
		}
		break;
	case EBI_GC_SWEEP_SYNC:
		if (ebi_thread_barrier(et)) {
			ebi_mark_globals(et);
			vm->gc_stage = EBI_GC_MARK;
		}
		break;
	}
	ebi_mutex_unlock(&vm->gc_mutex);
}

ebi_weak_ref ebi_make_weak_ref(ebi_thread *et, ebi_ptr void *ptr)
{
	ebi_vm *vm = et->vm;

	ebi_mutex_lock(&vm->weak_mutex);
	ebi_weak_ref ref = ebi_make_weak_ref_no_mutex(et, ptr);
	ebi_mutex_unlock(&vm->weak_mutex);

	return ref;
}

ebi_ptr void *ebi_resolve_weak_ref(ebi_thread *et, ebi_weak_ref ref)
{
	ebi_vm *vm = et->vm;

	ebi_mutex_lock(&vm->weak_mutex);
	void *ptr = ebi_resolve_weak_ref_no_mutex(et, ref);
	ebi_mutex_unlock(&vm->weak_mutex);

	return ptr;
}

ebi_symbol *ebi_intern(ebi_thread *et, const char *data, size_t length)
{
	ebi_vm *vm = et->vm;
	uint32_t hash = ebi_intern_hash(data, length);

	ebi_mutex_lock(&vm->weak_mutex);

	if (vm->num_intern_slots == vm->cap_intern_slots) {
		ebi_intern_rehash(et);
	}

	ebi_intern_slot *slots = vm->intern_slots;
	uint32_t mask = (uint32_t)vm->max_intern_slots - 1;
	uint32_t ix = hash & mask;
	uint32_t scan = 0;

	ebi_intern_slot displaced = { 0 };

	ebi_symbol *sym = NULL;
	for (;;) {
		ebi_intern_slot *ns = &slots[ix];
		uint32_t nscan = (ix - ns->hash) & mask;
		if (!ns->weak_ix || nscan < scan) {
			displaced = *ns;
			scan = nscan;

			ebi_symbol *sym = ebi_new_array_uninit(et, vm->types.symbol, length, data);

			memcpy(sym->data, data, length);
			sym->length = length;

			uint64_t weak = ebi_make_weak_ref_no_mutex(et, sym);

			ns->hash = hash;
			ns->weak_ix = (uint32_t)(weak >> 32);
			ns->weak_gen = (uint32_t)weak;

			break;
		} else if (ns->hash == hash) {
			uint64_t ref = (uint64_t)ns->weak_ix << 32 | (uint64_t)ns->weak_gen;
			void *obj = ebi_resolve_weak_ref_no_mutex(et, ref);
			if (obj && ebi_obj_count(obj) == length && !memcmp(obj, data, length)) {
				sym = (ebi_symbol*)obj;
				break;
			}
		}
		scan++;
		ix = (ix + 1) & mask;
	}

	while (displaced.weak_ix) {
		ebi_intern_slot *ns = &slots[ix];
		uint32_t nscan = (ix - ns->hash) & mask;
		if (!ns->weak_ix || nscan < scan) {
			*ns = displaced;
			displaced = *ns;
			scan = nscan;
		}
		scan++;
		ix = (ix + 1) & mask;
	}

	ebi_mutex_unlock(&vm->weak_mutex);

	return sym;
}

ebi_symbol *ebi_internz(ebi_thread *et, const char *data)
{
	return ebi_intern(et, data, strlen(data));
}


#endif
