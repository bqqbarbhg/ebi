#define _CRT_SECURE_NO_WARNINGS

#include "../src/ebi_core.h"
#include "../src/ebi_compiler.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	FILE *f = fopen(argv[1], "rb");
	fseek(f, 0, SEEK_END);
	size_t size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *data = (char*)malloc(size);
	fread(data, 1, size, f);
	fclose(f);

	ebi_vm *vm = ebi_make_vm();
	ebi_thread *et = ebi_make_thread(vm);

	ebi_ast *ast = ebi_parse(et, data, size);
	ebi_dump_ast(ast, 0);

	return 0;
}

