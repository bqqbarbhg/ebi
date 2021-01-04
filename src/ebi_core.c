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
	for (;;) {
		if (!_interlockedbittestandset((volatile long*)&m->lock, 0)) break;
		_InterlockedIncrement((volatile long*)&m->waiters);
		WaitOnAddress(&m->lock, &cmp, 4, INFINITE);
		_InterlockedDecrement((volatile long*)&m->waiters);
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

// Utility

static ebi_forceinline size_t ebi_grow_sz(size_t size, size_t min)
{
	return size > min ? size * 2 : min;
}

// -- Core

typedef struct ebi_obj ebi_obj;

// Object list

#define EBI_OBJLIST_SIZE 64

typedef struct ebi_objlist ebi_objlist;
struct ebi_objlist {
	ebi_objlist *next;
	ebi_obj *objs[EBI_OBJLIST_SIZE];
	uint32_t count;
};

typedef struct ebi_callstack ebi_callstack;
typedef struct ebi_callframe ebi_callframe;

struct ebi_obj {
	uint8_t epoch;
	uint32_t weak_slot;
	ebi_type *type;
	size_t count;
};

struct ebi_type {
	size_t size;
	ebi_arr uint32_t *slots;
	ebi_string name;
};

struct ebi_callframe {
	void *base;
	ebi_type *type;
	size_t count;
};

struct ebi_callstack {
	void *base;
	void *top;
	ebi_callframe *frames;
	uint32_t num_frames;
};

struct ebi_thread {
	ebi_vm *vm;
	uint32_t checkpoint;

	uint8_t epoch;
	ebi_mutex mutex;

	ebi_objlist *objs_mark;
	ebi_objlist *objs_alive;

	ebi_callstack stack;
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
	uint8_t epoch;
	uint8_t pad[11];

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

size_t ebi_obj_count(ebi_arr void *ptr)
{
	if (!ptr) return 0;
	ebi_obj *obj = (ebi_obj*)ptr - 1;
	return obj->count;
}

ebi_forceinline void ebi_mark(ebi_thread *et, void *ptr)
{
	if (!ptr) return;
	ebi_obj *obj = (ebi_obj*)ptr - 1;
	if (obj->epoch == et->epoch) return;
	obj->epoch = et->epoch;

	if (obj->type->slots) {
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

void ebi_mark_slots(ebi_thread *et, void *ptr, ebi_type *type, size_t count)
{
	ebi_mark(et, type);
	if (!type->slots) return;

	char *inst_ptr = (char*)ptr;

	size_t num_slots = ebi_obj_count(type->slots);
	size_t stride = type->size;
	uint32_t *begin = type->slots, *end = begin + num_slots;
	for (; count > 0; count--) {
		for (uint32_t *slot = begin; slot != end; slot++) {
			ebi_mark(et, *(void**)(inst_ptr + *slot));
		}
		inst_ptr += stride;
	}
}

void ebi_mark_globals(ebi_thread *et)
{
	ebi_vm *vm = et->vm;

	// Types
	size_t num_types = sizeof(ebi_types) / sizeof(ebi_type*);
	ebi_mark_slots(et, &vm->types, vm->types.object, num_types);
}

void *ebi_callstack_push(ebi_callstack *stack, ebi_type *type, size_t count)
{
	void *base = stack->top;
	size_t size = type->size * count;
	ebi_callframe *frame = &stack->frames[stack->num_frames++];
	memset(base, 0, size);
	frame->base = base;
	frame->type = type;
	frame->count = count;
	stack->top = (char*)base + size;
	return base;
}

void ebi_callstack_pop_check(ebi_callstack *stack, void *ptr)
{
	ebi_callframe *frame = &stack->frames[--stack->num_frames];
	ebi_assert(ptr == frame->base);
	stack->top = ptr;
}

void ebi_callstack_pop(ebi_callstack *stack)
{
	ebi_callframe *frame = &stack->frames[--stack->num_frames];
	stack->top = frame->base;
}

void ebi_callstack_mark(ebi_thread *et, ebi_callstack *stack)
{
	for (uint32_t i = 0; i < stack->num_frames; i++) {
		ebi_callframe *frame = &stack->frames[i];
		ebi_mark_slots(et, frame->base, frame->type, frame->count);
	}
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
		if (obj->epoch != et->epoch) {
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

void *ebi_new(ebi_thread *et, ebi_type *type, size_t count)
{
	size_t size = type->size * count;
	ebi_obj *obj = (ebi_obj*)malloc(sizeof(ebi_obj) + size);
	if (!obj) return NULL;

	void *data = obj + 1;
	memset(data, 0, size);

	obj->epoch = et->epoch;
	obj->weak_slot = 0;
	obj->type = type;
	obj->count = count;

	ebi_add_obj(et, obj);

	return data;
}

void *ebi_new_uninit(ebi_thread *et, ebi_type *type, size_t count)
{
	size_t size = type->size * count;
	ebi_obj *obj = (ebi_obj*)malloc(sizeof(ebi_obj) + size);
	if (!obj) return NULL;

	void *data = obj + 1;

	obj->epoch = et->epoch;
	obj->weak_slot = 0;
	obj->type = type;
	obj->count = count;

	ebi_add_obj(et, obj);

	return data;
}

void *ebi_new_copy(ebi_thread *et, ebi_type *type, size_t count, const void *init)
{
	size_t size = type->size * count;
	ebi_obj *obj = (ebi_obj*)malloc(sizeof(ebi_obj) + size);
	if (!obj) return NULL;

	void *data = obj + 1;
	memcpy(data, init, size);

	obj->epoch = et->epoch;
	obj->weak_slot = 0;
	obj->type = type;
	obj->count = count;

	ebi_add_obj(et, obj);
	ebi_mark_slots(et, data, type, count);

	return data;
}

void *ebi_push(ebi_thread *et, ebi_type *type, size_t count)
{
	return ebi_callstack_push(&et->stack, type, count);
}

void ebi_pop_check(ebi_thread *et, void *ptr)
{
	ebi_callstack_pop(&et->stack);
}

void ebi_pop(ebi_thread *et)
{
	ebi_callstack_pop(&et->stack);
}

void ebi_set(ebi_thread *et, void *slot, const void *value)
{
	ebi_mark(et, *(void**)slot);
	ebi_mark(et, (void*)value);
	*(void**)slot = (void*)value;
}

void ebi_set_string(ebi_thread *et, ebi_string *dst, const ebi_string *src)
{
	ebi_set(et, &dst->data, (void*)src->data);
	dst->begin = src->begin;
	dst->length = src->length;
}

ebi_type *ebi_new_type(ebi_thread *et, const ebi_type_desc *desc)
{
	ebi_type *type = ebi_new(et, et->vm->types.type, 1);
	type->size = desc->size;
	ebi_set_string(et, &type->name, &desc->name);
	ebi_set(et, &type->slots, desc->slots);
	return type;
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
	if (et->epoch != vm->epoch) {
		et->epoch = vm->epoch;
		ebi_callstack_mark(et, &et->stack);
	}
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
				// TODO: Batch these
				ebi_mutex_lock(&vm->weak_mutex);
				ebi_weak_slot *slot = &vm->weak_slots[obj->weak_slot];
				slot->val.next_free = vm->weak_free_head;
				vm->weak_free_head = obj->weak_slot;
				slot->gen++;
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

ebi_symbol ebi_intern(ebi_thread *et, const char *data, size_t length)
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

	void *result = NULL;
	for (;;) {
		ebi_intern_slot *ns = &slots[ix];
		uint32_t nscan = (ix - ns->hash) & mask;
		if (!ns->weak_ix || nscan < scan) {
			displaced = *ns;
			scan = nscan;

			char *copy = ebi_new_copy(et, vm->types.char_, length, data);
			uint64_t weak = ebi_make_weak_ref_no_mutex(et, copy);

			ns->hash = hash;
			ns->weak_ix = (uint32_t)(weak >> 32);
			ns->weak_gen = (uint32_t)weak;

			result = copy;
			break;
		} else if (ns->hash == hash) {
			uint64_t ref = (uint64_t)ns->weak_ix << 32 | (uint64_t)ns->weak_gen;
			void *obj = ebi_resolve_weak_ref_no_mutex(et, ref);
			if (obj && ebi_obj_count(obj) == length && !memcmp(obj, data, length)) {
				result = obj;
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

	ebi_symbol sym = { result };
	return sym;
}

ebi_symbol ebi_internz(ebi_thread *et, const char *data)
{
	return ebi_intern(et, data, strlen(data));
}

