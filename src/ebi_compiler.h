#pragma once

#include "ebi_core.h"

typedef struct ebi_token ebi_token;
typedef struct ebi_ast ebi_ast;

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
	EBI_KW_RETURN,

	EBI_TT_ERROR,
	EBI_TT_ERROR_UNCLOSED_STRING,
	EBI_TT_ERROR_BAD_UTF8,
	EBI_TT_ERROR_BAD_TOKEN,

	EBI_TT_COUNT,

} ebi_token_type;

typedef enum {

	EBI_AST_BLOCK,  // (block asts...)
	EBI_AST_LIST,   // (list asts...)
	EBI_AST_STRUCT, // (struct name generics fields)
	EBI_AST_DEF,    // (def name generics params return body)
	EBI_AST_FIELD,  // (field name type)
	EBI_AST_NAME,   // (name)
	EBI_AST_NULL,   // (null)
	EBI_AST_RETURN, // (return expr)
	EBI_AST_BINOP,  // (binop a b)

	EBI_AST_COUNT,

} ebi_ast_type;

struct ebi_token {
	ebi_token_type type;
	ebi_symbol symbol;
};

struct ebi_ast {
	ebi_token token;
	ebi_ast_type type;

	size_t num_nodes;
	union {
		ebi_ast *nodes;
		struct ebi_ast_struct *struct_;
		struct ebi_ast_def *def_;
		struct ebi_ast_field *field;
		struct ebi_ast_return *return_;
		struct ebi_ast_binop *binop;
	};
};

struct ebi_ast_struct { ebi_ast name, generics, fields; };
struct ebi_ast_def { ebi_ast name, generics, params, return_, body; };
struct ebi_ast_field { ebi_ast name, type; };
struct ebi_ast_return { ebi_ast expr; };
struct ebi_ast_binop { ebi_ast a, b; };

ebi_ast *ebi_parse(ebi_thread *et, const char *source, size_t length);


