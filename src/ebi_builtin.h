#pragma once

#include "ebi_core.h"

typedef struct ebi_list {
	ebi_ref *data;
	uint32_t size;
	uint32_t capacity;
} ebi_list;

ebi_error *ebi_list_push(ebi_vm *vm, void *ret, void *args);
ebi_error *ebi_list_pop(ebi_vm *vm, void *ret, void *args);

