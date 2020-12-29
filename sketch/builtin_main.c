#include "../src/ebi_builtin.h"

int main(int argc, char **argv)
{
	ebi_vm *vm = ebi_make_vm();

	ebi_type *int_type = ebi_alloc(vm, sizeof(ebi_type));
	int_type->size = sizeof(int);

	ebi_list *list = ebi_alloc(vm, sizeof(ebi_list));

	for (int i = 0; i < 10; i++) {
		ebi_list_push(vm, NULL, &(struct { ebi_type *t; ebi_list *self; int elem; }){
			.t = int_type, .self = list, .elem = i * i,
		});
	}

	return 0;
}
