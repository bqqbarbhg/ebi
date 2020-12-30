#include "ebi_core.h"

#include <stdlib.h>
#include <string.h>

#include <intrin0.h>

struct ebi_obj {
	uint8_t epoch;
};

struct ebi_thread {
	ebi_vm *vm;

	uint8_t epoch;

	uint32_t slots[8];

	uint32_t dirty[128];
	uint32_t num_dirty;
};

struct ebi_vm {
	ebi_obj *objs;

	ebi_thread *threads;
	uint32_t num_threads;

	uint8_t epoch;

	uint32_t *dirty;
	uint32_t num_dirty;
};

__declspec(thread) uint32_t ebi_thread_id;

void ebi_flush_dirty(ebi_thread *et)
{
	if (!et->num_dirty) return;

	et->num_dirty = 0;
}

void ebi_retain(ebi_thread *et, uint32_t id)
{
	ebi_vm *vm = et->vm;
	ebi_obj *obj = &vm->objs[id];
	if (obj->epoch != vm->epoch) {
		vm->objs[id].epoch = vm->epoch;
		if (et->num_dirty == 128) ebi_flush_dirty(et);
		et->dirty[et->num_dirty++] = id;
	}
}

void ebi_mark_roots(ebi_thread *et)
{
	for (uint32_t i = 0; i < 8; i++) {
		ebi_retain(et, et->slots[i]);
	}

	ebi_flush_dirty(et);
}

void ebi_lock_thread(ebi_thread *et)
{
	ebi_checkpoint(et);
}

void ebi_unlock_thread(ebi_thread *et)
{
	ebi_checkpoint(et);
	ebi_flush_dirty(et);
}

void ebi_checkpoint(ebi_thread *et)
{
	ebi_vm *vm = et->vm;
	if (et->epoch == vm->epoch) return;

	ebi_mark_roots(et);
}

void ebi_set_id(ebi_vm *vm, uint32_t *slot, uint32_t id)
{
	ebi_retain(vm, id);
	*slot = id;
}

#if 0

typedef struct {
	void *data;
} ebi_gc_obj;

#define EBI_GC_DIRTY_STRIDE_LOG2 5

struct ebi_vm {
	ebi_gc_obj *gc_objs;
	uint32_t *color_mask;
	uint32_t *dirty;
	uint32_t max_dirty;
};

#define ebi_atomic_or32(atomic, value) \
	_InterlockedOr((volatile long*)(atomic), (long)(value))
#define ebi_atomic_load32(atomic) \
	_InterlockedExchangeAdd((volatile long*)(atomic), (long)(0))
#define ebi_bsf32(p_index, mask) \
	_BitScanForward((unsigned long*)(p_index), (unsigned long)(mask))

__forceinline void ebi_mark_gray(ebi_vm *vm, uint32_t id)
{
	if (!id) return;
	uint32_t iw = id >> 4, ib = 1 << ((id & 15) << 1);
	uint32_t color = vm->color_mask[iw];
	if (((color | (color >> 1)) & ib) == 0) {
		uint32_t di = iw >> EBI_GC_DIRTY_STRIDE_LOG2;
		uint32_t dw = di >> 5, db = di & 31;
		ebi_atomic_or32(&vm->color_mask[iw], (long)ib);
		ebi_atomic_or32(&vm->dirty[dw], (long)db);
	}
}

void ebi_collect(ebi_vm *vm)
{
	for (uint32_t di = 0; di < vm->max_dirty; di++) {
		uint32_t begin, bits = vm->dirty[di];
		ebi_bsf32(&begin, bits);
		begin *= EBI_GC_DIRTY_STRIDE_LOG2;
		for (uint32_t oi = begin; oi < EBI_GC_DIRTY_STRIDE_LOG2; oi++) {
			uint32_t wi = begin + oi;
			uint32_t gray = vm->color_mask[wi];
			gray = gray & ~(gray >> 1) & 0x55555555;
			ebi_atomic_or32(&vm->color_mask[wi], gray << 1);
			while (gray) {
				uint32_t ix;
				ebi_bsf32(&ix, gray);
				ix >>= 1;
				gray &= gray - 1;
			}
		}


	}
}

void ebi_set_id(ebi_vm *vm, uint32_t *slot, uint32_t id)
{
	ebi_mark_gray(vm, *slot);
	ebi_mark_gray(vm, id);
	*slot = id;
}

#endif

#if 0

struct ebi_vm {
	ebi_obj *objs;
	uint32_t next_id;
};

ebi_vm *ebi_make_vm()
{
	ebi_vm *vm = malloc(sizeof(ebi_vm));
	if (!vm) return NULL;
	vm->next_id = 0;
	vm->objs = malloc(sizeof(ebi_obj) * 1000);
	return vm;
}

ebi_slice ebi_alloc(ebi_vm *vm, ebi_type type, size_t count)
{
	uint32_t id = ++vm->next_id;
	ebi_obj *obj = &vm->objs[id];
	void *data = malloc(count * type.info->size);
	obj->data = data;
	obj->refcount = 1;
	ebi_slice ref = { data, 0, count };
	return ref;
}

ebi_slice ebi_const(const void *data, uint32_t count)
{
	ebi_slice ref = { data, 0, count };
	return ref;
}

void ebi_string_set(ebi_string *dst, ebi_string value)
{
}

#endif

#if 0

const ebi_field ebi_field_fields[] = {
	{ &ebi_type_type, { "type", 0, 4 }, offsetof(ebi_field, type), offsetof(ebi_field, type_ref) },
	{ &ebi_type_string, { "name", 0, 4 }, offsetof(ebi_field, name), ~0u },
	{ &ebi_type_string, { "offset", 0, 4 }, offsetof(ebi_field, name), ~0u },
};

typedef struct ebi_page
{
	uintptr_t begin;
	uintptr_t size;

	uint32_t *offsets[];

} ebi_page;

typedef struct {
	uintptr_t begin;
	ebi_page *page;
} ebi_page_entry;

struct ebi_vm
{
	ebi_page_entry *pages;
	size_t page_mask;

	ebi_obj *objs;
	uint32_t next_free_ref;
};

ebi_slice ebi_alloc(ebi_vm *vm, ebi_type *type, size_t count)
{
	size_t tsz = type->size;
	size_t alloc_sz = tsz * count;
	void *data = malloc(alloc_sz);
	memset(data, 0, sizeof(data));

	uint32_t ref = ++vm->next_free_ref;
	ebi_obj *obj = &vm->objs[ref];
	obj->data = data;
	obj->count = count;
	obj->type = type;
	obj->refcount = 1;

	ebi_slice slice;
	slice.data = data;
	slice.data_ref = ref;
	slice.count = count;
	return slice;
}

void ebi_retain(ebi_vm *vm, ebi_ref ref)
{
	if (!ref) return;
	ebi_obj *obj = &vm->objs[ref];
	++obj->refcount;
}

void ebi_release(ebi_vm *vm, ebi_ref ref)
{
	if (!ref) return;
	ebi_obj *obj = &vm->objs[ref];
	if (--obj->refcount == 0) {
		free(obj->data);
	}
}

ebi_string ebi_const_string(ebi_vm *vm, const char *data, size_t length)
{
	ebi_string s;
	s.data = data;
	s.data_ref = 0;
	s.length = length;
	return s;
}

ebi_string ebi_const_stringz(ebi_vm *vm, const char *data)
{
	ebi_string s;
	s.data = data;
	s.data_ref = 0;
	s.length = strlen(data);
	return s;
}

ebi_string ebi_copy_string(ebi_vm *vm, const char *data, size_t length)
{
	ebi_slice s = ebi_alloc(vm, &ebi_type_char, length);
	ebi_string str;
	str.data = s.data;
	str.data_ref = s.data_ref;
	str.length = s.count;
	return str;
}

ebi_string ebi_copy_stringz(ebi_vm *vm, const char *data)
{
	return ebi_copy_string(vm, data, strlen(data));
}

#endif

#if 0

#define ebi_ptr_header(ptr) ((ebi_header*)(ptr) - 1)

typedef struct {
	ebi_header dummy_header;
	ebi_type type;
} ebi_internal_type;

typedef struct {
	ebi_vm vm;

	ebi_internal_type internal_types[7];

} ebi_vm_imp;

ebi_vm *ebi_make_vm()
{
	ebi_vm_imp *vmi = malloc(sizeof(ebi_vm_imp));
	if (!vmi) return NULL;
	memset(vmi, 0, sizeof(ebi_vm_imp));
	ebi_vm *vm = &vmi->vm;

	vm->types.type = &vmi->internal_types[0].type;
	vm->types.field = &vmi->internal_types[1].type;
	vm->types.char_ = &vmi->internal_types[2].type;
	vm->types.string = &vmi->internal_types[3].type;
	vm->types.u32 = &vmi->internal_types[4].type;
	vm->types.ptr_char = &vmi->internal_types[5].type;
	vm->types.ptr_field = &vmi->internal_types[6].type;

	for (size_t i = 0; i < 7; i++) {
		vmi->internal_types[i].dummy_header.vm = vm;
		vmi->internal_types[i].dummy_header.type = vm->types.type;
		vmi->internal_types[i].dummy_header.refcount = 1;
		vmi->internal_types[i].dummy_header.count = 1;
	}

	vm->types.type->name = ebi_make_stringz(vm, "Type");
	vm->types.type->size = sizeof(ebi_type);
	vm->types.field->name = ebi_make_stringz(vm, "Field");
	vm->types.field->size = sizeof(ebi_field);
	vm->types.char_->name = ebi_make_stringz(vm, "Char");
	vm->types.char_->size = sizeof(char);
	vm->types.string->name = ebi_make_stringz(vm, "String");
	vm->types.string->size = sizeof(ebi_string);
	vm->types.u32->name = ebi_make_stringz(vm, "U32");
	vm->types.u32->size = sizeof(ebi_string);
	vm->types.ptr_char->name = ebi_make_stringz(vm, "Ptr[Char]");
	vm->types.ptr_char->size = sizeof(void*);
	vm->types.ptr_field->name = ebi_make_stringz(vm, "Ptr[Field]");
	vm->types.ptr_field->size = sizeof(void*);
	{
		ebi_field *fs = ebi_new(vm, vm->types.type, 3);
		vm->types.field->fields = fs;
		fs[0].name = ebi_make_stringz(vm, "size");
		fs[0].type = (ebi_type*)ebi_retain(vm->types.u32);
		fs[0].offset = offsetof(ebi_type, size);
		fs[1].name = ebi_make_stringz(vm, "name");
		fs[1].type = (ebi_type*)ebi_retain(vm->types.string);
		fs[1].offset = offsetof(ebi_type, name);
		fs[2].name = ebi_make_stringz(vm, "fields");
		fs[2].type = (ebi_type*)ebi_retain(vm->types.ptr_field);
		fs[2].offset = offsetof(ebi_type, fields);
	}

	{
		ebi_field *fs = ebi_new(vm, vm->types.field, 3);
		vm->types.field->fields = fs;
		fs[0].name = ebi_make_stringz(vm, "name");
		fs[0].type = (ebi_type*)ebi_retain(vm->types.string);
		fs[0].offset = offsetof(ebi_field, name);
		fs[1].name = ebi_make_stringz(vm, "type");
		fs[1].type = (ebi_type*)ebi_retain(vm->types.type);
		fs[1].offset = offsetof(ebi_field, type);
		fs[2].name = ebi_make_stringz(vm, "offset");
		fs[2].type = (ebi_type*)ebi_retain(vm->types.u32);
		fs[2].offset = offsetof(ebi_field, offset);
	}

	{
		ebi_field *fs = ebi_new(vm, vm->types.field, 3);
		vm->types.string->fields = fs;
		fs[0].name = ebi_make_stringz(vm, "data");
		fs[0].type = (ebi_type*)ebi_retain(vm->types.ptr_char);
		fs[1].name = ebi_make_stringz(vm, "begin");
		fs[1].type = (ebi_type*)ebi_retain(vm->types.u32);
		fs[2].name = ebi_make_stringz(vm, "end");
		fs[2].type = (ebi_type*)ebi_retain(vm->types.u32);
	}

	return vm;
}

ebi_ptr void *ebi_new(ebi_vm *vm, ebi_type *type, size_t count)
{
	ebi_retain(type);
	size_t type_sz = type->size;
	size_t data_sz = type_sz * count;
	size_t alloc_sz = data_sz + sizeof(ebi_header);

	void *data = malloc(alloc_sz);
	if (!data) return NULL;

#if EBI_DEBUG
	vm->debug_allocated_bytes += alloc_sz;
#endif

	ebi_header* head = (ebi_header*)data;
	void *ptr = head + 1;

	head->vm = vm;
	head->type = type;
	head->refcount = 1;
	head->count = (uint32_t)count;

	memset(ptr, 0, data_sz);

	return ptr;
}

ebi_ptr void *ebi_retain(ebi_ptr void *ptr)
{
	if (!ptr) return ptr;
	ebi_header *head = ebi_ptr_header(ptr);
	head->refcount++;
	return ptr;
}

void ebi_release(ebi_ptr void *ptr)
{
	if (!ptr) return;
	ebi_header *head = ebi_ptr_header(ptr);
	uint32_t count = --head->refcount;
	if (count == 0) {
		size_t tsz = head->type->size;
#if EBI_DEBUG
		head->vm->debug_allocated_bytes -= head->count * tsz + sizeof(ebi_header);
#endif
		ebi_release(head->type);

		head->vm = NULL;
		free(head);
	}
}

uint32_t ebi_count(ebi_ptr void *ptr)
{
	if (!ptr) return 0;
	ebi_header *head = ebi_ptr_header(ptr);
	return head->count;
}

bool ebi_check(ebi_ptr void *ptr, size_t index)
{
	if (!ptr) return false;
	ebi_header *head = ebi_ptr_header(ptr);
	return index < head->count;
}

ebi_string ebi_make_string(ebi_vm *vm, const char *data, size_t length)
{
	ebi_assert(length <= UINT32_MAX);

	char *alloc = (char*)ebi_new(vm, vm->types.char_, length);
	if (!alloc) {
		ebi_string null_str = { NULL };
		return null_str;
	}

	memcpy(alloc, data, length);

	ebi_string s;
	s.data = alloc;
	s.begin = 0;
	s.end = (uint32_t)length;
	return s;
}

ebi_string ebi_make_stringz(ebi_vm *vm, const char *data)
{
	return ebi_make_string(vm, data, strlen(data));
}

#endif

#if 0

const char ebi_msg_bounds[] = "Bounds check";

#define ebi_boundsret(ptr, end_offset) \
	if (!ebi_check((ptr), (end_offset))) \
		return ebi_fail(ebi_msg_bounds);

#if EBI_DEBUG
	ebi_header *ebi_ptr_header(const void *ptr) {
		ebi_header* head = (ebi_header*)ptr - 1;
		return head;
	}
#else
	#define ebi_ptr_header(ptr) ((ebi_header*)(ptr) - 1)
#endif

ebi_vm *ebi_make_vm()
{
	return NULL;
}

ebi_ref *ebi_alloc(ebi_vm *vm, size_t size)
{
	ebi_assert(size <= UINT32_MAX);

	void *data = malloc(size + sizeof(ebi_header));
	if (!data) return NULL;

	ebi_header* head = (ebi_header*)data;
	void *ptr = head + 1;

	head->vm = vm;
	head->refcount = 1;
	head->size = (uint32_t)size;

	memset(ptr, 0, size);

	return ptr;
}

void ebi_retain(ebi_ref *ptr)
{
	if (!ptr) return;
	ebi_header *head = ebi_ptr_header(ptr);
	head->refcount++;
}

void ebi_release(ebi_ref *ptr)
{
	if (!ptr) return;
	ebi_header *head = ebi_ptr_header(ptr);
	uint32_t count = --head->refcount;
	if (count == 0) {
		head->vm = NULL;
		free(head);
	}
}

bool ebi_check(ebi_ref *ptr, size_t end_offset)
{
	if (end_offset == 0) return true;
	if (!ptr) return false;
	ebi_header *head = ebi_ptr_header(ptr);
	return end_offset <= head->size;
}

ebi_string ebi_make_string(const char *data)
{
}

ebi_error *ebi_make_error(const char *file, uint32_t line, const char *msg)
{

}

ebi_error *ebi_string_slice(ebi_vm *vm, void *ret, void *args)
{
	ebi_string_slice_args *a = (ebi_string_slice_args*)args;
	ebi_string *r = (ebi_string*)ret;

	if (a->begin > a->end) return ebi_fail("Bad range");
	uint32_t begin = a->self.begin + a->begin;
	uint32_t end = a->self.begin + a->end;
	if (end > a->self.end) return ebi_fail("Bad range");

	if (a->begin == a->end) {
		ebi_string null_str = { NULL, 0, 0 };
		*r = null_str;
		return NULL;
	}

	ebi_retain(a->self.data);
	r->data = a->self.data;
	r->begin = begin;
	r->end = end;
	return NULL;
}

#endif
