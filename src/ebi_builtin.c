#include "ebi_builtin.h"

#include <string.h>

const char ebi_msg_oom[] = "Out of memory";
const char ebi_msg_bounds[] = "Bounds check";

#define ebi_boundsret(ptr, end_offset) \
	if (!ebi_check((ptr), (end_offset))) \
		return ebi_fail(ebi_msg_bounds);

ebi_error *ebi_list_push(ebi_vm *vm, void *ret, void *args)
{
	typedef struct { ebi_type *t; ebi_list *self; char elem[0]; } argt;
	argt *a = (argt*)args;
	ebi_type *t = a->t;
	size_t tsz = t->size;
	ebi_list *self = a->self;
	void *data = self->data;

	if (self->size >= self->capacity) {
		ebi_boundsret(data, self->size * tsz);

		uint32_t cap = self->capacity * 2;
		if (cap < 8) cap = 8;
		void *new_data = ebi_alloc(vm, cap * tsz);
		if (!new_data) return ebi_fail(ebi_msg_oom);

		memcpy(data, new_data, self->size * tsz);

		self->data = new_data;
		self->capacity = cap;
		data = new_data;
	}

	size_t off = self->size * tsz;
	ebi_boundsret(data, off + tsz);
	ebi_type_assign(t, (char*)data + off, a->elem);

	self->size += 1;

	return NULL;
}

ebi_error *ebi_list_pop(ebi_vm *vm, void *ret, void *args)
{
	typedef struct { ebi_type *t; ebi_list *self; } argt;
	argt *a = (argt*)args;
	ebi_type *t = a->t;
	size_t tsz = t->size;
	ebi_list *self = a->self;
	void *data = self->data;

	if (self->size == 0) return ebi_fail("Empty list pop");
	self->size -= 1;

	size_t off = self->size * tsz;
	ebi_boundsret(data, off + tsz);
	memcpy(ret, (char*)data + off, tsz);

	return NULL;
}
