#pragma once

#include "ebi_core.h"

typedef struct ebi_compiler ebi_compiler;
typedef struct ebi_token ebi_token;

typedef enum {

	EBI_TT_LINE_END,
	EBI_TT_FILE_END,

	EBI_TT_IDENT,
	EBI_TT_NUMBER,
	EBI_TT_STRING,

	EBI_TT_DOT,
	EBI_TT_COLON,
	EBI_TT_COMMA,
	EBI_TT_LPAREN, EBI_TT_RPAREN,
	EBI_TT_LCURLY, EBI_TT_RCURLY,
	EBI_TT_LSQUARE, EBI_TT_RSQUARE,

	EBI_TT_ADD, EBI_TT_ADD_ASSIGN,
	EBI_TT_SUB, EBI_TT_SUB_ASSIGN,
	EBI_TT_MUL, EBI_TT_MUL_ASSIGN,
	EBI_TT_DIV, EBI_TT_DIV_ASSIGN,
	EBI_TT_MOD, EBI_TT_MOD_ASSIGN,
	EBI_TT_LT, EBI_TT_LT_EQ,
	EBI_TT_GT, EBI_TT_GT_EQ,
	EBI_TT_NOT, EBI_TT_NOT_EQ,
	EBI_TT_ASSIGN, EBI_TT_EQUAL,

	EBI_TT_RD_ARROW,

	EBI_KW_DEF,
	EBI_KW_LET,
	EBI_KW_STRUCT,
	EBI_KW_CLASS,

	EBI_TT_ERROR,
	EBI_TT_ERROR_UNCLOSED_STRING,
	EBI_TT_ERROR_BAD_UTF8,
	EBI_TT_ERROR_BAD_TOKEN,

	EBI_TT_COUNT,

} ebi_token_type;

struct ebi_token {
	ebi_token_type type;
	ebi_symbol symbol;
};

void ebi_compile(ebi_thread *et, const char *source, size_t length);
