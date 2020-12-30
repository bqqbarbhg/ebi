#include "ebi_core.h"

#include <stdlib.h>
#include <string.h>

#include <intrin.h>
#include <Windows.h>

// DEBUG
#include <stdio.h>

// -- OS abstraction

// Platform

#define ebi_forceinline __forceinline

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
		if (!_interlockedbittestandset((volatile long*)&m->lock, 1)) break;
		_InterlockedIncrement((volatile long*)&m->waiters);
		WaitOnAddress(&m->lock, &cmp, 4, INFINITE);
		_InterlockedDecrement((volatile long*)&m->waiters);
	}
}

bool ebi_mutex_try_lock(ebi_mutex *m)
{
	return !_interlockedbittestandset((volatile long*)&m->lock, 1);
}

void ebi_mutex_unlock(ebi_mutex *m)
{
	_InterlockedExchange((volatile long*)&m->lock, 0);
	if (m->waiters > 0) {
		WakeByAddressSingle(&m->lock);
	}
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

ebi_forceinline size_t ebi_obj_count(void *ptr)
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

	printf("Mark %p (%.*s)\n", obj,
		(int)obj->type->name.length,
		(char*)obj->type->name.data + obj->type->name.begin);

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
	ebi_mark_slots(et, &vm->types, vm->types.ptr_type, num_types);
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
		ebi_type *t = vm->types.ptr_type;
		const uint32_t slots[] = { 0 };
		t->name = ebi_new_stringz(et, "ref ebi.Type");
		t->size = sizeof(ebi_type*);
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
		vm->max_threads = vm->max_threads * 2;
		if (vm->max_threads == 0) vm->max_threads = 16;
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
			printf("FREE: %p\n", obj);
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
		printf("MARK\n");
		if (!mark) {
			vm->checkpoint++;
			vm->gc_stage = EBI_GC_MARK_SYNC;
		}
		break;
	case EBI_GC_MARK_SYNC:
		printf("MARK_SYNC\n");
		if (mark) {
			vm->gc_stage = EBI_GC_MARK;
		} else if (ebi_thread_barrier(et) && ebi_ia_get_count(&vm->objs_mark) == mark_count) {
			vm->gc_stage = EBI_GC_SWEEP;
			ebi_ia_push_all(&vm->objs_sweep, ebi_ia_pop_all(&vm->objs_alive));
		}
		break;
	case EBI_GC_SWEEP:
		printf("SWEEP\n");
		if (!sweep) {
			vm->checkpoint++;
			vm->epoch++;
			vm->gc_stage = EBI_GC_SWEEP_SYNC;
		}
		break;
	case EBI_GC_SWEEP_SYNC:
		printf("SWEEP_SYNC\n");
		if (ebi_thread_barrier(et)) {
			ebi_mark_globals(et);
			vm->gc_stage = EBI_GC_MARK;
		}
		break;
	}
	ebi_mutex_unlock(&vm->gc_mutex);
}

#if 0

#define ebi_forceinline __forceinline

struct ebi_type {
	ebi_obj obj;
	ebi_string name;
	ebi_size size;
	uint32_t num_slots;
	ebi_obj *slots_obj;
};

#define EBI_MAX_MARKS 128

typedef struct {
	uintptr_t head;
	uintptr_t counter;
} ebi_ia_stack;

void ebi_ia_push(ebi_ia_stack *s, void *ptr)
{
	ebi_ia_stack r = *s;
	for (;;) {
		*(void**)ptr = (void*)r.head;
		unsigned char ok = _InterlockedCompareExchange128(
			(volatile long long*)s,
			(long long)(r.counter + 1),
			(long long)ptr,
			(long long*)&r);
		if (ok) return;
	}
}

void ebi_ia_push_all(ebi_ia_stack *s, void *ptr)
{
	ebi_ia_stack r = *s;
	for (;;) {
		ebi_assert(r.head == 0);
		unsigned char ok = _InterlockedCompareExchange128(
			(volatile long long*)s,
			(long long)(r.counter + 1),
			(long long)ptr,
			(long long*)&r);
		if (ok) return;
	}
}

void *ebi_ia_pop(ebi_ia_stack *s)
{
	ebi_ia_stack r = *s;
	for (;;) {
		void **head = (void**)r.head;
		if (!head) return NULL;
		unsigned char ok = _InterlockedCompareExchange128(
			(volatile long long*)s,
			(long long)(r.counter + 1),
			(long long)((uintptr_t)*head),
			(long long*)&r);
		if (ok) {
			*(void**)head = NULL;
			return head;
		}
	}
}

void *ebi_ia_pop_all(ebi_ia_stack *s)
{
	ebi_ia_stack r = *s;
	for (;;) {
		void **head = (void**)r.head;
		if (!head) return NULL;
		unsigned char ok = _InterlockedCompareExchange128(
			(volatile long long*)s,
			(long long)(r.counter + 1),
			(long long)(NULL),
			(long long*)&r);
		if (ok) return head;
	}
}

typedef struct ebi_marks ebi_marks;

struct ebi_marks {
	ebi_marks *next;
	ebi_obj *objs[EBI_MAX_MARKS];
	uint32_t count;
};

typedef enum {
	EBI_GC_MARK,
	EBI_GC_WAIT_MARK,
	EBI_GC_SWEEP,
	EBI_GC_WAIT_SWEEP,
} ebi_gc_stage;

struct ebi_vm {
	uint32_t checkpoint;
	uint8_t epoch;
	uint8_t pad[11];

	ebi_ia_stack marks_gc;
	ebi_ia_stack marks_sweep;
	ebi_ia_stack marks_alive;
	ebi_ia_stack marks_reuse;

	ebi_gc_stage gc_stage;

	ebi_thread *main_thread;

	ebi_type type_type;
	ebi_type type_u32;
	ebi_type type_char;

	ebi_thread **threads;
	uint32_t num_threads;

	void *alloc;
};

struct ebi_thread {
	ebi_vm *vm;
	uint8_t epoch;

	uint32_t running;
	uint32_t checkpoint;

	ebi_marks *marks;
	ebi_marks *alive;

	ebi_obj **stack;
	uint32_t stack_top;

	void *alloc;
};

ebi_marks *ebi_alloc_marks(ebi_vm *vm)
{
	ebi_marks *marks = ebi_ia_pop(&vm->marks_reuse);
	if (!marks) {
		marks = (ebi_marks*)malloc(sizeof(ebi_marks));
		marks->count = 0;
		marks->next = NULL;
	}
	return marks;
}

ebi_marks *ebi_flush_marks(ebi_thread *et)
{
	ebi_vm *vm = et->vm;
	if (et->marks->count > 0) {
		ebi_ia_push(&vm->marks_gc, et->marks);
		et->marks = ebi_alloc_marks(vm);
	}
	return et->marks;
}

ebi_marks *ebi_flush_alive(ebi_thread *et)
{
	ebi_vm *vm = et->vm;
	if (et->alive->count > 0) {
		ebi_ia_push(&vm->marks_alive, et->alive);
		et->alive = ebi_alloc_marks(vm);
	}
	return et->alive;
}

ebi_forceinline static void ebi_mark(ebi_thread *et, ebi_obj *obj)
{
	if (!obj || obj->epoch == et->epoch) return;
	obj->epoch = et->epoch;
	ebi_marks *marks = et->marks;
	if (marks->count == EBI_MAX_MARKS) {
		marks = ebi_flush_marks(et);
	}
	marks->objs[marks->count++] = obj;
}

ebi_forceinline static void ebi_add_obj(ebi_thread *et, ebi_obj *obj)
{
	ebi_assert(obj);
	obj->epoch = et->epoch;
	ebi_marks *alive = et->alive;
	if (alive->count == EBI_MAX_MARKS) {
		alive = ebi_flush_alive(et);
	}
	alive->objs[alive->count++] = obj;
}

void ebi_mark_globals(ebi_thread *et)
{
	ebi_vm *vm = et->vm;
	ebi_mark(et, &vm->type_type.obj);
	ebi_mark(et, &vm->type_u32.obj);
}

ebi_vm *ebi_make_vm()
{
	void *alloc = malloc(sizeof(ebi_vm) + 128);
	if (!alloc) return NULL;
	ebi_vm *vm = (ebi_vm*)((char*)alloc + (64 - (uintptr_t)alloc%64));
	memset(vm, 0, sizeof(ebi_vm));
	vm->alloc = alloc;

	vm->threads = (ebi_thread**)malloc(16 * sizeof(ebi_thread*));

	vm->type_u32.obj.type = &vm->type_type;
	vm->type_u32.obj.count = 1;
	vm->type_u32.size = sizeof(uint32_t);

	vm->type_u32.obj.type = &vm->type_type;
	vm->type_u32.obj.count = 1;
	vm->type_char.size = sizeof(char);

	ebi_thread *et = ebi_make_thread(vm);
	vm->main_thread = et;

	ebi_begin(et);

	vm->type_type.obj.type = &vm->type_type;
	vm->type_type.obj.count = 1;
	vm->type_type.size = sizeof(ebi_type);
	vm->type_type.num_slots = 2;
	vm->type_type.slots_obj = ebi_alloc(et, &vm->type_u32, 2);
	{
		uint32_t *slots = (uint32_t*)(vm->type_type.slots_obj + 1);
		slots[0] = offsetof(ebi_type, slots_obj) / sizeof(ebi_obj*);
		slots[1] = offsetof(ebi_type, name) / sizeof(ebi_obj*);
	}

	vm->type_u32.name = ebi_make_strz(et, "U32");
	vm->type_char.name = ebi_make_strz(et, "Char");
	vm->type_type.name = ebi_make_strz(et, "Type");

	ebi_end(et);

	return vm;
}

ebi_thread *ebi_make_thread(ebi_vm *vm)
{
	void *alloc = malloc(sizeof(ebi_thread) + 128);
	if (!alloc) return NULL;
	ebi_thread *et = (ebi_thread*)((char*)alloc + (64 - (uintptr_t)alloc%64));
	memset(et, 0, sizeof(ebi_thread));
	et->alloc = alloc;

	et->vm = vm;
	et->epoch = vm->epoch;
	et->marks = ebi_alloc_marks(vm);
	et->alive = ebi_alloc_marks(vm);

	et->stack = (ebi_obj**)malloc(sizeof(ebi_obj*) * 128);
	memset(et->stack, 0, sizeof(et->stack));

	vm->threads[vm->num_threads++] = et;

	return et;
}

bool ebi_gc_marks(ebi_thread *et)
{
	ebi_vm *vm = et->vm;
	ebi_marks *marks = ebi_ia_pop(&vm->marks_gc);
	if (!marks) return false;

	for (uint32_t oi = 0; oi < marks->count; oi++) {
		ebi_obj *obj = marks->objs[oi];
		ebi_type *type = obj->type;

		ebi_mark(et, &type->obj);

		ebi_obj **slot_ptr = (ebi_obj**)(obj + 1);
		size_t stride = obj->count / type->size;
		size_t num_insts = obj->count, num_slots = type->num_slots;
		uint32_t *slots = (uint32_t*)(type->slots_obj + 1);
		if (num_slots == 0) continue;

		for (size_t ii = 0; ii < num_insts; ii++) {
			for (size_t si = 0; si < num_slots; si++) {
				ebi_mark(et, slot_ptr[slots[si]]);
			}
			slot_ptr += stride;
		}
	}

	marks->count = 0;
	ebi_ia_push(&vm->marks_reuse, marks);

	return true;
}

bool ebi_gc_sweep(ebi_thread *et)
{
	ebi_vm *vm = et->vm;
	ebi_marks *alive = ebi_ia_pop(&vm->marks_sweep);
	if (!alive) return false;

	uint8_t bad_epoch = (uint8_t)(vm->epoch - 2);
	for (uint32_t oi = 0; oi < alive->count; oi++) {
		ebi_obj *obj = alive->objs[oi];
		if (obj->epoch != bad_epoch) {
			ebi_add_obj(et, obj);
		} else {
			free(obj);
		}
	}

	alive->count = 0;
	ebi_ia_push(&vm->marks_reuse, alive);

	return true;
}

bool ebi_sync_threads(ebi_vm *vm)
{
	for (uint32_t i = 0; i < vm->num_threads; i++) {
		ebi_thread *ot = vm->threads[i];
		if (ot->running && ot->checkpoint != vm->checkpoint) return false;
	}
	return true;
}

void ebi_gc_step(ebi_thread *et)
{
	ebi_vm *vm = et->vm;
	ebi_checkpoint(et);
	if (vm->gc_stage == EBI_GC_MARK) {
		if (!ebi_gc_marks(et)) {
			ebi_flush_marks(et);
			if (!ebi_gc_marks(et)) {
				vm->checkpoint++;
				vm->gc_stage = EBI_GC_WAIT_MARK;
			}
		}
	} else if (vm->gc_stage == EBI_GC_WAIT_MARK) {
		bool sync = ebi_sync_threads(vm);
		bool mark = ebi_gc_marks(et);
		if (mark) {
			vm->gc_stage = EBI_GC_MARK;
		} else if (sync) {
			ebi_ia_push_all(&vm->marks_sweep, ebi_ia_pop_all(&vm->marks_alive));
			vm->gc_stage = EBI_GC_SWEEP;
		}
	} else if (vm->gc_stage == EBI_GC_SWEEP) {
		ebi_assert(!ebi_gc_marks(et));
		if (ebi_gc_sweep(et)) return;

		vm->checkpoint++;
		vm->epoch++;
		vm->gc_stage = EBI_GC_WAIT_SWEEP;
	} else if (vm->gc_stage == EBI_GC_WAIT_SWEEP) {
		if (ebi_sync_threads(vm)) {
			for (uint32_t i = 0; i < vm->num_threads; i++) {
				ebi_thread *ot = vm->threads[i];
				if (ot->running || ot->checkpoint == vm->checkpoint) continue;

				uint32_t top = ot->stack_top;
				for (uint32_t i = 0; i < top; i++) {
					ebi_mark(et, et->stack[i]);
				}
			}

			ebi_mark_globals(et);

			vm->gc_stage = EBI_GC_MARK;
		}
	} else {
		ebi_assert(0 && "Bad stage");
	}
}

void ebi_set_obj(ebi_thread *et, ebi_obj **slot, ebi_obj *value)
{
	ebi_mark(et, *slot);
	ebi_mark(et, value);
	*slot = value;
}

void ebi_begin(ebi_thread *et)
{
	et->running = 1;
	ebi_checkpoint(et);
}

void ebi_end(ebi_thread *et)
{
	ebi_checkpoint(et);
	ebi_flush_marks(et);
	ebi_flush_alive(et);
	et->running = 0;
}

void ebi_checkpoint_heavy(ebi_thread *et)
{
	ebi_vm *vm = et->vm;
	ebi_flush_marks(et);
	ebi_flush_alive(et);
	if (et->epoch != vm->epoch) {
		et->epoch = vm->epoch;
		uint32_t top = et->stack_top;
		for (uint32_t i = 0; i < top; i++) {
			ebi_mark(et, et->stack[i]);
		}
	}
	et->checkpoint = vm->checkpoint;
}

void ebi_checkpoint(ebi_thread *et)
{
	if (et->vm->checkpoint != et->checkpoint) ebi_checkpoint_heavy(et);
}

ebi_obj *ebi_alloc(ebi_thread *et, ebi_type *type, size_t count)
{
	size_t size = sizeof(ebi_obj) + type->size * count;
	ebi_obj *obj = (ebi_obj*)malloc(size);
	if (!obj) return NULL;
	memset(obj, 0, size);

	obj->type = type;
	obj->count = count;
	obj->epoch = et->epoch;

	ebi_add_obj(et, obj);

	return obj;
}

ebi_obj **ebi_push(ebi_thread *et, uint32_t num)
{
	et->stack_top += num;
	return et->stack + (et->stack_top - num);
}

void ebi_pop(ebi_thread *et, uint32_t num)
{
	et->stack_top -= num;
	memset(et->stack + et->stack_top, 0, sizeof(uint32_t) * num);
}

ebi_type *ebi_make_type(ebi_thread *et, ebi_string name, size_t size, const uint32_t *slots, uint32_t num_slots)
{
	ebi_vm *vm = et->vm;
	ebi_obj *obj = ebi_alloc(et, &vm->type_type, 1);
	ebi_type *type = (ebi_type*)obj;

	type->name = name;
	type->size = size;
	type->num_slots = num_slots;
	type->slots_obj = ebi_alloc(et, &vm->type_u32, num_slots);
	memcpy(type->slots_obj + 1, slots, num_slots * sizeof(uint32_t));

	return type;
}

ebi_string ebi_make_str(ebi_thread *et, const char *data, size_t length)
{
	ebi_vm *vm = et->vm;
	ebi_obj *copy = ebi_alloc(et, &vm->type_char, length);
	memcpy(copy + 1, data, length);
	ebi_string s = { copy, sizeof(ebi_obj), length };
	return s;
}

ebi_string ebi_make_strz(ebi_thread *et, const char *data)
{
	return ebi_make_str(et, data, strlen(data));
}

#endif
