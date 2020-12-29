#pragma once

#include <stdint.h>
#include <stdbool.h>

// TEMP
#define EBI_DEBUG 1
#define ebi_assert(cond) do { if (!(cond)) __debugbreak(); } while (0)

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
