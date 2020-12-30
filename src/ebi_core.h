#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct ebi_vm ebi_vm;
typedef struct ebi_obj ebi_obj;
typedef struct ebi_thread ebi_thread;

void ebi_set_id(ebi_vm *vm, uint32_t *slot, uint32_t id);

#if 0

void ebi_set_id(ebi_vm *vm, uint32_t *slot, uint32_t id);

#endif

#if 0

typedef struct ebi_vm ebi_vm;
typedef struct ebi_obj ebi_obj;
typedef struct ebi_slice ebi_slice;
typedef struct ebi_type ebi_type;
typedef struct ebi_type_info ebi_type_info;
typedef struct ebi_string ebi_string;

struct ebi_obj {
	void *data;
	uint32_t refcount;
};

struct ebi_slice {
	void *data;
	uint32_t id;
	uint32_t count;
};

struct ebi_type_info {
	uint32_t size;
};

struct ebi_type {
	ebi_type_info *info;
	uint32_t id;
	uint32_t count;
};

struct ebi_string {
	char *data;
	uint32_t id;
	uint32_t length;
};

ebi_vm *ebi_make_vm();

ebi_slice ebi_alloc(ebi_vm *vm, ebi_type type, size_t count);
ebi_slice ebi_const(const void *data, uint32_t count);

void ebi_string_set(ebi_string *dst, ebi_string value);

#endif

#if 0

typedef struct ebi_vm ebi_vm;
typedef struct ebi_obj ebi_obj;
typedef struct ebi_slice ebi_slice;
typedef struct ebi_type ebi_type;
typedef struct ebi_type_info ebi_type_info;

struct ebi_obj {
	void *data;
	uint32_t refcount;
};

struct ebi_slice {
	void *data;
	uint32_t id;
	uint32_t count;
};

struct ebi_type_info {
	uint32_t size;
};

struct ebi_type {
	ebi_type_info *info;
	uint32_t id;
	uint32_t count;
};

struct ebi_string {
	char *data;
	uint32_t id;
	uint32_t length;
};

ebi_vm *ebi_make_vm();

ebi_slice ebi_alloc(ebi_vm *vm, ebi_type type, size_t count);
ebi_slice ebi_const(const void *data, uint32_t count);

void ebi_retain(ebi_vm *vm, uint32_t id);
void ebi_release(ebi_vm *vm, uint32_t id);

#endif

#if 0

typedef struct ebi_vm ebi_vm;
typedef struct ebi_obj ebi_obj;
typedef struct ebi_ref ebi_ref;
typedef struct ebi_type ebi_type;
typedef struct ebi_type_struct ebi_type_struct;
typedef struct ebi_array ebi_array;
typedef struct ebi_slice ebi_slice;
typedef struct ebi_string ebi_string;
typedef struct ebi_field ebi_field;

typedef uint32_t ebi_ref;

struct ebi_string {
	char *data;
	ebi_ref data_ref;
	uint32_t length;
};

struct ebi_obj {
	void *data;
	ebi_type *type;
	uint32_t count;
	uint32_t refcount;
};

struct ebi_slice {
	void *data;
	ebi_ref data_ref;
	uint32_t count;
};

struct ebi_type {
	uint32_t size;
	ebi_string name;
};

struct ebi_field {
	const ebi_type *type;
	ebi_string name;
	uint32_t offset;
	uint32_t ref_offset;
	ebi_ref type_ref;
};

struct ebi_type_struct {
	ebi_type type;

	ebi_field *fields;
	uint32_t fields_count;

	ebi_ref fields_ref;
};

const ebi_type_struct ebi_type_type;
const ebi_type_struct ebi_type_field;
const ebi_type_struct ebi_type_string;

ebi_slice ebi_alloc(ebi_vm *vm, ebi_type *type, size_t count);

void ebi_retain(ebi_vm *vm, ebi_ref ref);
void ebi_release(ebi_vm *vm, ebi_ref ref);

ebi_string ebi_const_string(ebi_vm *vm, const char *data, size_t length);
ebi_string ebi_const_stringz(ebi_vm *vm, const char *data);

ebi_string ebi_copy_string(ebi_vm *vm, const char *data, size_t length);
ebi_string ebi_copy_stringz(ebi_vm *vm, const char *data);

#endif

#if 0

// TEMP
#define EBI_DEBUG 1
#define ebi_assert(cond) do { if (!(cond)) __debugbreak(); } while (0)

#define ebi_ptr

typedef struct ebi_vm ebi_vm;
typedef struct ebi_header ebi_header;
typedef struct ebi_type ebi_type;
typedef struct ebi_field ebi_field;
typedef struct ebi_param ebi_param;
typedef struct ebi_string ebi_string;

struct ebi_header {
	ebi_vm *vm;
	ebi_ptr ebi_type *type;
	uint32_t refcount;
	uint32_t count;
};

struct ebi_string {
	ebi_ptr char *data;
	uint32_t begin, end;
};

struct ebi_field {
	ebi_string name;
	ebi_type *type;
	uint32_t offset;
};

struct ebi_param {
	ebi_string name;
	ebi_type *type;
};

struct ebi_type {
	uint32_t size;
	ebi_string name;
	ebi_ptr ebi_field *fields;
	ebi_ptr ebi_param *params;
};

typedef struct ebi_types {
	ebi_type *type;
	ebi_type *field;
	ebi_type *char_;
	ebi_type *string;
	ebi_type *u32;
	ebi_type *ptr_char;
	ebi_type *ptr_field;
} ebi_types;

struct ebi_vm {
	ebi_types types;

#if EBI_DEBUG
	size_t debug_allocated_bytes;
#endif
};

ebi_vm *ebi_make_vm();

ebi_ptr void *ebi_new(ebi_vm *vm, ebi_type *type, size_t count);

ebi_ptr void *ebi_retain(ebi_ptr void *ptr);
void ebi_release(ebi_ptr void *ptr);

uint32_t ebi_count(ebi_ptr void *ptr);
bool ebi_check(ebi_ptr void *ptr, size_t index);

ebi_string ebi_make_string(ebi_vm *vm, const char *data, size_t length);
ebi_string ebi_make_stringz(ebi_vm *vm, const char *data);

#endif

#if 0

typedef struct ebi_vm ebi_vm;

ebi_vm *ebi_make_vm();

typedef struct ebi_header {
	ebi_vm *vm;
	uint32_t size;
	uint32_t refcount;
} ebi_header;

typedef void ebi_ref;
typedef void ebi_error;

#define ebi_obj

ebi_ref *ebi_alloc(ebi_vm *vm, size_t size);

void ebi_retain(ebi_ref *ptr);
void ebi_release(ebi_ref *ptr);

bool ebi_check(ebi_ref *ptr, size_t end_offset);

typedef struct ebi_string {
	ebi_obj char *data;
	uint32_t begin, end;
} ebi_string;

typedef struct ebi_type {
	uint32_t size;
} ebi_type;

void ebi_type_assign(ebi_type *t, void *ptr, void *src);

ebi_string ebi_make_string(const char *data);

ebi_error *ebi_make_error(const char *file, uint32_t line, const char *msg);

#define ebi_fail(msg) ebi_make_error(__FILE__, __LINE__, (msg))

typedef struct { ebi_string self; uint32_t begin; uint32_t end; } ebi_string_slice_args;
ebi_error *ebi_string_slice(ebi_vm *vm, void *ret, void *args)

#define ebi_call(func, ...) \
	i_err = func(i_vm, &ret, &(func##_args){ __VA_ARGS__ })

#endif
