#include "../src/ebi_core.h"

#include <stdio.h>

#include <Windows.h>

typedef struct pair_t pair_t;
struct pair_t {

	pair_t *left;
	pair_t *right;
};

DWORD WINAPI ThreadProc(
  _In_ LPVOID lpParameter
)
{
	ebi_vm *vm = (ebi_vm*)lpParameter;
	ebi_thread *et = ebi_make_thread(vm);

	ebi_lock_thread(et);

	for (;;) {
		ebi_gc_step(et);
	}

	ebi_unlock_thread(et);
}

int main(int argc, char **argv)
{
	ebi_vm *vm = ebi_make_vm();
	ebi_thread *et = ebi_make_thread(vm);
	ebi_types *ts = ebi_get_types(vm);

	CreateThread(NULL, 0, ThreadProc, vm, 0, NULL);

	ebi_lock_thread(et);

	ebi_type **pt_pair = ebi_push(et, ts->object, 1);


	ebi_type_desc *desc = ebi_push(et, ts->type_desc, 1);
	desc->size = sizeof(pair_t);
	desc->name = ebi_new_stringz(et, "pair_t");
	desc->slots = ebi_new(et, ts->u32, 2);
	desc->slots[0] = offsetof(pair_t, left);
	desc->slots[1] = offsetof(pair_t, right);

	*pt_pair = ebi_new_type(et, desc);

	ebi_pop_check(et, desc);

	pair_t **root = ebi_push(et, ts->object, 1);

	root[0] = ebi_new(et, *pt_pair, 1);
	root[0]->left = ebi_new(et, *pt_pair, 1);
	root[0]->right = root[0];

	printf("pt_pair: %p\n", pt_pair);
	printf("*pt_pair: %p\n", *pt_pair);
	printf("root: %p\n", root[0]);
	printf("root->left: %p\n", root[0]->left);
	printf("root->right: %p\n", root[0]->right);

	ebi_unlock_thread(et);
	Sleep(1000);
	ebi_lock_thread(et);

	ebi_pop_check(et, root);

	ebi_unlock_thread(et);
	Sleep(1000);

	ebi_lock_thread(et);
	for (;;) {
		ebi_checkpoint(et);
		ebi_new(et, *pt_pair, 1);
	}
	ebi_unlock_thread(et);

	return 0;
}
