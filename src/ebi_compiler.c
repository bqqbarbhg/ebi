#include "ebi_compiler.h"

#include <string.h>

// Copyright (c) 2008-2009 Bjoern Hoehrmann <bjoern@hoehrmann.de>
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
//
// See http://bjoern.hoehrmann.de/utf-8/decoder/dfa/ for details.

#define EBI_UTF8_ACCEPT 0
#define EBI_UTF8_REJECT 1

static const uint8_t ebi_utf8d[] = {
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 00..1f
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 20..3f
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 40..5f
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 60..7f
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, // 80..9f
	7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7, // a0..bf
	8,8,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2, // c0..df
	0xa,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x4,0x3,0x3, // e0..ef
	0xb,0x6,0x6,0x6,0x5,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8, // f0..ff
	0x0,0x1,0x2,0x3,0x5,0x8,0x7,0x1,0x1,0x1,0x4,0x6,0x1,0x1,0x1,0x1, // s0..s0
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,1,1,1,1,1,0,1,0,1,1,1,1,1,1, // s1..s2
	1,2,1,1,1,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1, // s3..s4
	1,2,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,3,1,3,1,1,1,1,1,1, // s5..s6
	1,3,1,1,1,1,1,3,1,3,1,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1, // s7..s8
};

ebi_forceinline static uint32_t
ebi_utf8_validate(uint32_t state, uint32_t byte) {
	uint32_t type = ebi_utf8d[byte];
	state = ebi_utf8d[256 + state*16 + type];
	return state;
}

ebi_forceinline static uint32_t
ebi_utf8_decode(uint32_t state, uint32_t* codep, uint32_t byte) {
	uint32_t type = ebi_utf8d[byte];

	*codep = (state != 0) ?
		(byte & 0x3fu) | (*codep << 6) :
		(0xff >> type) & (byte);

	state = ebi_utf8d[256 + state*16 + type];
	return state;
}

// Dense bitset of allowed identifier characters below U+0100.
static uint32_t ebi_ident_char_dense_bits[] = {
	0x00000000, 0x03ff0000, 0x07fffffe, 0x07fffffe,
	0x00000000, 0x77bca500, 0xff7fffff, 0xff7fffff,
};

// Valid identifier character range endpoints. The range starts as non-included
// each stop switches between included and non-included range. Eg. 0x0100 starts
// a valid range and 0x1680 starts an invalid range defining U+0100 to U+167F as
// valid.
static uint16_t ebi_ident_binary_stops[] = {
	0x0100, 0x1680, 0x1681, 0x180e, 0x180f, 0x2000, 0x200b, 0x200e,
	0x202a, 0x202f, 0x203f, 0x2041, 0x2054, 0x2055, 0x2060, 0x2070,
	0x2070, 0x2190, 0x2460, 0x2500, 0x2776, 0x2794, 0x2c00, 0x2e00,
	0x2e80, 0x3000, 0x3004, 0x3008, 0x3021, 0x3030, 0x3031, 0xd800,
	0xf900, 0xfd3e, 0xfd40, 0xfdd0, 0xfdf0, 0xfe45, 0xfe47, 0xfffe,
};

// Is Unicode `codep` an allowed extended identifier character.
static ebi_forceinline bool ebi_is_identifier(uint32_t codep) {
	if (codep < 0x100) {
		// U+0000 to U+00FF: Dense codep bitmap
		uint32_t word = ebi_ident_char_dense_bits[codep >> 5];
		return (bool)((word >> (codep & 31)) & 1);
	} else if (codep < 0x10000) {
		// U+0100 to U+FFFF: Binary search ranges
		uint32_t lo = 0, hi = ebi_arraycount(ebi_ident_binary_stops);
		while (lo < hi) {
			uint32_t mid = (lo + hi) >> 1;
			if ((uint32_t)ebi_ident_binary_stops[mid] < codep) {
				lo = mid + 1;
			} else {
				hi = mid;
			}
		}
		return (lo & 1) == 0;
	} else {
		// U+10000 and up: Repetetive structure
		return codep < 0xF0000 && (codep & 0xFFFF) <= 0xFFFD;
	}
}

#define EBI_IDENT_CAHCE_SIZE 2048
#define EBI_IDENT_CAHCE_SCAN 32

const char *const ebi_tt_names[] = {
	"end of line", "end of file",
	"identifier", "number", "string",
	"'.'", "':'", "','",
	"'('", "')'", "'{'", "'}'", "'['", "']'",
	"'+'", "'+='", "'-'", "'-='", "'*'", "'*='", "'/'", "'/='", "'%'", "'%='",
	"'<'", "'<='", "'>'", "'>='", "'!'", "'!='", "'='", "'=='",
	"'=>'",
	"keyword 'def'", "keyword 'let'", "keyword 'struct'", "keyword 'class'",
	"keyword 'return'",
	"error", "unclosed string", "bad utf-8", "bad token",
};
ebi_static_assert(tt_name_count, ebi_arraycount(ebi_tt_names) == EBI_TT_COUNT);

typedef struct {
	uint32_t hash;
	uint32_t lru;
	ebi_symbol symbol;
} ebi_cached_ident;

typedef struct {
	ebi_thread *et;

	ebi_token prev_token;
	ebi_token token;

	const char *src_ptr;
	const char *src_end;

	uint32_t ident_lru;
	ebi_cached_ident ident_cache[EBI_IDENT_CAHCE_SIZE];
} ebi_parser;

ebi_symbol ebi_compiler_intern(ebi_parser *ep, const char *data, size_t length, uint32_t hash)
{
	uint32_t lru = ++ep->ident_lru;
	uint32_t ix = hash & (EBI_IDENT_CAHCE_SIZE - 1);
	uint32_t best_delta = UINT32_MAX;
	uint32_t insert_ix = 0;
	for (uint32_t scan = 0; scan < EBI_IDENT_CAHCE_SCAN; scan++) {
		ebi_cached_ident *ci = &ep->ident_cache[ix];
		uint32_t delta = lru - ci->lru;
		if (!ci->symbol.data) {
			insert_ix = ix;
			break;
		} else if (ci->hash == hash && ebi_obj_count(ci->symbol.data) == length
			&& !memcmp(data, ci->symbol.data, length)) {
			ci->lru = lru;
			return ci->symbol;
		} else if (delta < best_delta) {
			insert_ix = ix;
			best_delta = delta;
		}

		ix = (ix + 1) & (EBI_IDENT_CAHCE_SIZE - 1);
	}

	ebi_cached_ident *ci = &ep->ident_cache[insert_ix];
	ebi_symbol sym = ebi_intern(ep->et, data, length);

	ci->hash = hash;
	ci->lru = lru;
	ci->symbol = sym;

	return sym;
}

ebi_forceinline bool ebi_is_space(char c)
{
	// python: hex(sum(1<<(ord(c)-1) for c in " \t\v\f\r"))
	const uint32_t mask = 0x80001d00;
	uint32_t uc = (uint32_t)c - 1;
	return uc < 32 ? (mask >> uc) & 1 : false;
}

typedef struct {
	uint32_t hash;
	uint32_t length;
	const char *name;
	ebi_token_type kw;
} ebi_kw_mapping;

// Generated by misc/make_kw_map.py
static const uint32_t ebi_kw_shift = 22;
static const ebi_kw_mapping ebi_kw_map[] = {

	{ 0xc03fd006, 5, "class", EBI_KW_CLASS },
	{ 0xde78cd07, 3, "def", EBI_KW_DEF },
	{ 0x648bdbca, 6, "return", EBI_KW_RETURN },
	{ 0x40eb9ead, 3, "let", EBI_KW_LET },
	{ 0xbf3f8e8f, 6, "struct", EBI_KW_STRUCT }, { 0 }, { 0 }, { 0 },
};

void ebi_scan(ebi_parser *ep)
{
	ebi_token_type tt;
	const char *sp = ep->src_ptr;
	ptrdiff_t left = ep->src_end - sp;
	char c = left > 0 ? sp[0] : '\0';
	char nc = left > 1 ? sp[1] : '\0';
	bool nc_eq = nc == '=';

	while (ebi_is_space(c)) {
		sp++; left--;
		c = nc;
		nc = left > 1 ? sp[1] : '\0';
	}

	ep->prev_token = ep->token;
	ep->token.symbol.data = NULL;

	ep->src_ptr = sp;
	sp++; left--;
	switch (c) {

	case '\0': tt = EBI_TT_FILE_END; break;
	case '\n': tt = EBI_TT_LINE_END; break;
	case ';': tt = EBI_TT_LINE_END; break;
	case '.': tt = EBI_TT_DOT; break;
	case ':': tt = EBI_TT_COLON; break;
	case ',': tt = EBI_TT_COMMA; break;
	case '(': tt = EBI_TT_LPAREN; break;
	case ')': tt = EBI_TT_RPAREN; break;
	case '{': tt = EBI_TT_LCURLY; break;
	case '}': tt = EBI_TT_RCURLY; break;
	case '[': tt = EBI_TT_LSQUARE; break;
	case ']': tt = EBI_TT_RSQUARE; break;
	case '+': tt = EBI_TT_ADD + nc_eq; sp += nc_eq; break;
	case '-': tt = EBI_TT_SUB + nc_eq; sp += nc_eq; break;
	case '*': tt = EBI_TT_MUL + nc_eq; sp += nc_eq; break;
	case '/': tt = EBI_TT_DIV + nc_eq; sp += nc_eq; break;
	case '%': tt = EBI_TT_MOD + nc_eq; sp += nc_eq; break;
	case '<': tt = EBI_TT_LT + nc_eq; sp += nc_eq; break;
	case '>': tt = EBI_TT_GT + nc_eq; sp += nc_eq; break;
	case '!': tt = EBI_TT_NOT + nc_eq; sp += nc_eq; break;
	case '=':
		tt = nc == '>' ? EBI_TT_RD_ARROW : EBI_TT_ASSIGN + nc_eq;
		sp += nc_eq | (nc == '>');
		break;

	case '"': {
		tt = EBI_TT_STRING;
		c = nc;
		nc = left > 1 ? sp[1] : '\0';
		uint32_t u8s = 0;
		for (;;) {
			if (c == '"') {
				break;
			} else if (c == '\\') {
			} else if (c == '\0') {
				tt = EBI_TT_ERROR_UNCLOSED_STRING;
				break;
			} else {
				u8s = ebi_utf8_validate(u8s, c);
				if (u8s == EBI_UTF8_REJECT) {
					tt = EBI_TT_ERROR_BAD_UTF8;
					break;
				}
				c = nc;
				nc = left > 2 ? sp[2] : '\0';
				sp++; left--;
			}
			sp++;
		}
	} break;

	case '0': case '1': case '2': case '3': case '4':
	case '5': case '6': case '7': case '8': case '9':
		tt = EBI_TT_NUMBER;
		break;

	default: {
		tt = EBI_TT_IDENT;
		uint32_t hash = 0;
		uint32_t codep = 0;
		uint32_t u8s = 0;
		for (;;) {
			u8s = ebi_utf8_decode(u8s, &codep, c);
			c = nc;
			if (u8s == EBI_UTF8_REJECT) {
				tt = EBI_TT_ERROR_BAD_UTF8;
				break;
			} else if (u8s == EBI_UTF8_ACCEPT) {
				if (!ebi_is_identifier(codep)) {
					break;
				} else {
					hash = (hash ^ codep) * 0x9e3779b9;
				}
			}
			sp++; left--;
			nc = left > 0 ? sp[0] : '\0';
		}

		size_t len = --sp - ep->src_ptr;
		if (len == 0) {
			if (tt == EBI_TT_IDENT) {
				tt = EBI_TT_ERROR_BAD_TOKEN;
			}
		}

		if (tt == EBI_TT_IDENT) {
			uint32_t kw_mask = (ebi_arraycount(ebi_kw_map) - 1);
			const ebi_kw_mapping *kw = &ebi_kw_map[(hash >> ebi_kw_shift) & kw_mask];
			if (kw->hash == hash && kw->length == len
				&& !memcmp(kw->name, ep->src_ptr, len)) {
				tt = kw->kw;
			}
		}

		if (tt == EBI_TT_IDENT) {
			 ep->token.symbol = ebi_compiler_intern(ep, ep->src_ptr, len, hash);
		}

	} break;

	}

	ep->token.type = tt;
	ep->src_ptr = sp;
}

ebi_forceinline bool ebi_accept(ebi_parser *ep, ebi_token_type tt)
{
	if (ep->token.type == tt) {
		ebi_scan(ep);
		return true;
	} else {
		return false;
	}
}

void ebi_push_ast(ebi_parser *ep, ebi_ast_type type, const ebi_token *token)
{
}

void ebi_pop_ast(ebi_parser *ep)
{
}

void ebi_add_ast(ebi_parser *ep, ebi_ast_type type, const ebi_token *token)
{
}

void ebi_perror(ebi_parser *ep, const char *msg, ...)
{
}

bool ebi_parse_name(ebi_parser *ep, const char *hint)
{
	if (!ebi_accept(ep, EBI_TT_IDENT)) {
		ebi_perror(ep, "Expected name for %s", hint);
		return false;
	}

	ebi_add_ast(ep, EBI_AST_NAME, &ep->prev_token);

	return true;
}

bool ebi_parse_type(ebi_parser *ep)
{
	if (ebi_accept(ep, EBI_TT_LPAREN)) {
		ebi_parse_type(ep);
		if (!ebi_accept(ep ,EBI_TT_RPAREN)) {
			ebi_perror(ep, "Expected a closing ')'");
			return false;
		}
	} else if (ebi_accept(ep, EBI_TT_IDENT)) {
		ebi_add_ast(ep, EBI_AST_NAME, &ep->prev_token);
	} else {
		ebi_perror(ep, "Expected a type");
	}

	return true;
}

bool ebi_parse_generics(ebi_parser *ep)
{
	if (ebi_accept(ep, EBI_TT_LSQUARE)) {
		ebi_push_ast(ep, EBI_AST_LIST, &ep->prev_token);

		if (ebi_accept(ep, EBI_TT_RSQUARE)) {
			ebi_perror(ep, "Generic list cannot be empty");
			return false;
		}

		do {
			ebi_parse_name(ep, "generic parameter");
		} while (ebi_accept(ep, EBI_TT_COMMA));

		if (!ebi_accept(ep, EBI_TT_RSQUARE)) {
			ebi_perror(ep, "Expected closing ']' or another parameter");
			return false;
		}

		ebi_pop_ast(ep);
	} else {
		ebi_add_ast(ep, EBI_AST_LIST, NULL);
	}

	return true;
}

bool ebi_parse_fields(ebi_parser *ep)
{
	ebi_push_ast(ep, EBI_AST_LIST, &ep->prev_token);

	while (!ebi_accept(ep, EBI_TT_RCURLY)) {
		if (ebi_accept(ep, EBI_TT_LINE_END)) continue;

		if (ebi_accept(ep, EBI_TT_IDENT)) {
			ebi_push_ast(ep, EBI_AST_FIELD, &ep->prev_token);
			ebi_add_ast(ep, EBI_AST_NAME, &ep->prev_token);

			if (!ebi_accept(EBI_TT_COLON)) {
				ebi_perror(ep, "Expected ':' before type");
				return false;
			}

			ebi_parse_type(ep);

			ebi_pop_ast(ep);
		} else {
			ebi_perror(ep, "Expected a field");
			return false;
		}
	}

	ebi_pop_ast(ep);

	return true;
}

bool ebi_parse_struct(ebi_parser *ep)
{
	ebi_push_ast(ep, EBI_AST_STRUCT, &ep->prev_token);

	ebi_parse_name(ep, "struct");
	ebi_parse_generics(ep);

	if (ebi_accept(ep, EBI_TT_LCURLY)) {
		ebi_parse_fields(ep);
	} else {
		ebi_add_ast(ep, EBI_AST_NULL, NULL);
	}

	ebi_pop_ast(ep);
}

bool ebi_parse_param_defs(ebi_parser *ep)
{
	if (!ebi_accept(ep, EBI_TT_LPAREN)) {
		ebi_perror(ep, "Expected '(' for parameters");
		return false;
	}

}

bool ebi_parse_block(ebi_parser *ep, ebi_token_type end);

bool ebi_parse_def(ebi_parser *ep)
{
	ebi_push_ast(ep, EBI_AST_DEF, &ep->prev_token);

	ebi_parse_name(ep, "function");
	ebi_parse_generics(ep);
	ebi_parse_param_defs(ep);

	if (ebi_accept(ep, EBI_TT_COLON)) {
		ebi_parse_type(ep);
	} else {
		ebi_add_ast(ep, EBI_AST_NULL, NULL);
	}

	if (ebi_accept(ep, EBI_TT_LCURLY)) {
		ebi_push_ast(ep, EBI_AST_BLOCK, &ep->prev_token);
		ebi_parse_block(ep, EBI_TT_RCURLY);
		ebi_pop_ast(ep);
	}

	ebi_pop_ast(ep);
}

bool ebi_parse_statement(ebi_parser *ep)
{
	if (ebi_accept(ep, EBI_TT_LCURLY)) {
		ebi_push_ast(ep, EBI_AST_BLOCK, &ep->prev_token);
		ebi_parse_block(ep, EBI_TT_RCURLY);
		ebi_pop_ast(ep);
	} else if (ebi_accept(ep, EBI_KW_RETURN)) {
	}
}

bool ebi_parse_block(ebi_parser *ep, ebi_token_type end)
{
	while (!ebi_accept(ep, end)) {
		if (ebi_accept(ep, EBI_TT_LINE_END)) continue;

		if (ebi_accept(ep, EBI_KW_STRUCT)) {
			ebi_parse_struct(ep);
		} else if (ebi_accept(ep, EBI_KW_DEF)) {
			ebi_parse_def(ep);
		} else {
			ebi_parse_statement(ep);
		}
	}
}

bool ebi_parse_root(ebi_parser *ep)
{
	ebi_push_ast(ep, EBI_AST_BLOCK, NULL);

	ebi_parse_block(ep, EBI_TT_FILE_END);

	ebi_pop_ast(ep);
}

// DEBUG
#include <stdio.h>

ebi_ast *ebi_parse(ebi_thread *et, const char *source, size_t length)
{
	ebi_parser ep = { et };
	ep.src_ptr = source;
	ep.src_end = source + length;

	for (;;) {
		ebi_scan(&ep);
		if (ep.token.symbol.data) {
			printf("%s: %.*s\n", ebi_tt_names[ep.token.type],
				(int)ebi_obj_count(ep.token.symbol.data),
				ep.token.symbol.data);
		} else {
			printf("%s\n", ebi_tt_names[ep.token.type]);
		}
		if (ep.token.type == EBI_TT_FILE_END) break;
	}
}
