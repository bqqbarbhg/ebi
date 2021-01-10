#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define ebi_assert(cond) do { if (!(cond)) __debugbreak(); } while (0)
#define ebi_static_assert(name, cond) typedef int ebi_assert_##name[(cond) ? 1 : -1]
#define ebi_arraycount(arr) (sizeof(arr)/sizeof(*(arr)))
#define ebi_ptr
#define ebi_arr

#define ebi_forceinline __forceinline

#define EBI_DEBUG 1

typedef struct ebi_vm ebi_vm;
typedef struct ebi_thread ebi_thread;
typedef struct ebi_type_info ebi_type_info;
typedef struct ebi_field_info ebi_field_info;
typedef struct ebi_type ebi_type;
typedef struct ebi_field ebi_field;
typedef struct ebi_string ebi_string;
typedef struct ebi_type_desc ebi_type_desc;
typedef struct ebi_types ebi_types;
typedef struct ebi_symbol ebi_symbol;
typedef uint64_t ebi_weak_ref;

struct ebi_string {
	ebi_arr void *data;
	size_t begin;
	size_t length;
};

struct ebi_type_desc {
	size_t size;
	ebi_string name;
	ebi_arr uint32_t *slots;
};

struct ebi_types {
	ebi_type_info *array_info;

	ebi_type *type;
	ebi_type *char_;
	ebi_type *u32;
	ebi_type *string;
	ebi_type *object;
	ebi_type *type_desc;
	ebi_type *symbol;
};

struct ebi_symbol {
	size_t length;
	char data[];
};

struct ebi_field_info {
	ebi_symbol *name;
	ebi_type *type;
	uint32_t generic_index;
};

struct ebi_type_info {
	size_t num_total_fields;
	ebi_symbol *name;
	ebi_field_info fields[];
};

typedef enum {
	EBI_FIELD_IS_REF = 0x1,
	EBI_FIELD_HAS_REFS = 0x2,
} ebi_field_flags;

struct ebi_field {
	ebi_type *type;
	uint32_t offset;
	uint32_t flags;
};

typedef enum {
	EBI_TYPE_IS_REF = 0x1,
	EBI_TYPE_HAS_REFS = 0x2,
	EBI_TYPE_HAS_SUFFIX = 0x4,
} ebi_type_flags;

struct ebi_type {
	size_t num_total_fields;
	ebi_type_info *info;
	uint32_t flags;
	uint32_t ref_size;
	uint32_t data_size;
	uint32_t elem_size;
	uint32_t num_fields;
	ebi_field fields[];
};

ebi_vm *ebi_make_vm();
ebi_thread *ebi_make_thread(ebi_vm *vm);
ebi_types *ebi_get_types(ebi_vm *vm);

void *ebi_new(ebi_thread *et, ebi_type *type);
void *ebi_new_uninit(ebi_thread *et, ebi_type *type);

void *ebi_new_array(ebi_thread *et, ebi_type *type, size_t count);
void *ebi_new_array_uninit(ebi_thread *et, ebi_type *type, size_t count);

void ebi_set(ebi_thread *et, void *inst, size_t offset, void *value);
void ebi_set_string(ebi_thread *et, ebi_string *dst, const ebi_string *src);

ebi_type *ebi_new_type(ebi_thread *et, const ebi_type_desc *desc);

ebi_string ebi_new_string(ebi_thread *et, const char *data, size_t length);
ebi_string ebi_new_stringz(ebi_thread *et, const char *data);

void ebi_lock_thread(ebi_thread *et);
void ebi_unlock_thread(ebi_thread *et);

void ebi_checkpoint(ebi_thread *et);

void ebi_gc_assist(ebi_thread *et);
void ebi_gc_step(ebi_thread *et);

ebi_weak_ref ebi_make_weak_ref(ebi_thread *et, ebi_ptr void *ptr);
ebi_ptr void *ebi_resolve_weak_ref(ebi_thread *et, ebi_weak_ref ref);

ebi_symbol *ebi_intern(ebi_thread *et, const char *data, size_t length);
ebi_symbol *ebi_internz(ebi_thread *et, const char *data);
