#include "ebi_core.h"

#include <stdlib.h>
#include <string.h>

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
