#include "../src/ebi_core.h"

typedef struct pair_t pair_t;
struct pair_t {

	pair_t *left;
	pair_t *right;
};

int main(int argc, char **argv)
{
	ebi_vm *vm = ebi_make_vm();
	ebi_thread *et = ebi_make_thread(vm);
	ebi_types *ts = ebi_get_types(vm);

	ebi_lock_thread(et);

	ebi_type **pt_pair = ebi_push(et, ts->ptr_type, 1);

	ebi_type_desc *desc = ebi_push(et, ts->type_desc, 1);
	desc->size = sizeof(pair_t);
	desc->name = ebi_new_stringz(et, "pair_t");
	desc->slots = ebi_new(et, ts->u32, 2);
	desc->slots[0] = offsetof(pair_t, left);
	desc->slots[1] = offsetof(pair_t, right);

	*pt_pair = ebi_new_type(et, desc);

	ebi_pop_check(et, desc);

	pair_t *root = ebi_push(et, *pt_pair, 1);

	root->left = ebi_new(et, *pt_pair, 1);
	root->right = ebi_new(et, *pt_pair, 1);
	root->right = ebi_new(et, *pt_pair, 1);

	printf("pt_pair: %p\n", pt_pair);
	printf("*pt_pair: %p\n", *pt_pair);
	printf("root: %p\n", root);
	printf("root->left: %p\n", root->left);
	printf("root->right: %p\n", root->right);

	for (uint32_t i = 0; i < 20; i++) {
		ebi_gc_step(et);
	}

	ebi_unlock_thread(et);

	return 0;
}
