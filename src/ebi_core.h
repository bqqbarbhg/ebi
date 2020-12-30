#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define ebi_assert(cond) do { if (!(cond)) __debugbreak(); } while (0)
#define ebi_arraycount(arr) (sizeof(arr)/sizeof(*(arr)))
#define ebi_ptr
#define ebi_arr

typedef struct ebi_vm ebi_vm;
typedef struct ebi_thread ebi_thread;
typedef struct ebi_type ebi_type;
typedef struct ebi_string ebi_string;
typedef struct ebi_type_desc ebi_type_desc;
typedef struct ebi_types ebi_types;

struct ebi_string {
	ebi_ptr void *data;
	size_t begin;
	size_t length;
};

struct ebi_type_desc {
	size_t size;
	ebi_string name;
	ebi_arr uint32_t *slots;
};

struct ebi_types {
	ebi_type *type;
	ebi_type *char_;
	ebi_type *u32;
	ebi_type *string;
	ebi_type *ptr_type;
	ebi_type *type_desc;
};

ebi_vm *ebi_make_vm();
ebi_thread *ebi_make_thread(ebi_vm *vm);
ebi_types *ebi_get_types(ebi_vm *vm);

void *ebi_new(ebi_thread *et, ebi_type *type, size_t count);
void *ebi_new_uninit(ebi_thread *et, ebi_type *type, size_t count);
void *ebi_new_copy(ebi_thread *et, ebi_type *type, size_t count, const void *init);

void *ebi_push(ebi_thread *et, ebi_type *type, size_t count);
void ebi_pop_check(ebi_thread *et, void *ptr);
void ebi_pop(ebi_thread *et);

void ebi_set(ebi_thread *et, void *slot, const void *value);
void ebi_set_string(ebi_thread *et, ebi_string *dst, const ebi_string *src);

ebi_type *ebi_new_type(ebi_thread *et, const ebi_type_desc *desc);

ebi_string ebi_new_string(ebi_thread *et, const char *data, size_t length);
ebi_string ebi_new_stringz(ebi_thread *et, const char *data);

void ebi_lock_thread(ebi_thread *et);
void ebi_unlock_thread(ebi_thread *et);

void ebi_checkpoint(ebi_thread *et);

void ebi_gc_assist(ebi_thread *et);
void ebi_gc_step(ebi_thread *et);

#if 0

typedef struct ebi_vm ebi_vm;
typedef struct ebi_thread ebi_thread;
typedef struct ebi_obj ebi_obj;
typedef struct ebi_type ebi_type;
typedef struct ebi_string ebi_string;
typedef uintptr_t ebi_offset;
typedef size_t ebi_size;

struct ebi_obj {
	uint8_t epoch;
	ebi_type *type;
	ebi_size count;
};

struct ebi_string {
	ebi_obj *data_obj;
	size_t offset;
	size_t length;
};

ebi_vm *ebi_make_vm();
ebi_thread *ebi_make_thread(ebi_vm *vm);

void ebi_begin(ebi_thread *et);
void ebi_end(ebi_thread *et);
void ebi_checkpoint(ebi_thread *et);
void ebi_set_obj(ebi_thread *et, ebi_obj **slot, ebi_obj *value);
ebi_obj *ebi_alloc(ebi_thread *et, ebi_type *type, size_t count);

ebi_obj **ebi_push(ebi_thread *et, uint32_t num);
void ebi_pop(ebi_thread *et, uint32_t num);

ebi_type *ebi_make_type(ebi_thread *et, ebi_string name, size_t size, const uint32_t *slots, uint32_t num_slots);
ebi_string ebi_make_str(ebi_thread *et, const char *data, size_t length);
ebi_string ebi_make_strz(ebi_thread *et, const char *data);

void ebi_gc_step(ebi_thread *et);


#endif
