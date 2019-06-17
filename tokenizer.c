#include <stdint.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>

#define MAX_TOK_LEN 4096

struct tokenizer {
	FILE *input;
	uint32_t line;
	uint32_t column;
	char buf[MAX_TOK_LEN];
};

enum tokentype {
	TT_IDENTIFIER,
	TT_CHAR_LIT,
	TT_STRING_LIT,
	TT_ELLIPSIS,
	TT_HEX_INT_LIT,
	TT_OCT_INT_LIT,
	TT_DEC_INT_LIT,
	TT_SEP,
	/* errors and similar */
	TT_UNKNOWN,
	TT_OVERFLOW,
	TT_EOF,
};

static const char* tokentype_to_str(enum tokentype tt) {
	switch(tt) {
		case TT_IDENTIFIER: return "iden";
		case TT_CHAR_LIT: return "char";
		case TT_STRING_LIT: return "string";
		case TT_ELLIPSIS: return "ellipsis";
		case TT_HEX_INT_LIT: return "hexint";
		case TT_OCT_INT_LIT: return "octint";
		case TT_DEC_INT_LIT: return "decint";
		case TT_SEP: return "separator";
		case TT_UNKNOWN: return "unknown";
		case TT_OVERFLOW: return "overflow";
		case TT_EOF: return "eof";
	}
	return "????";
}

struct token {
	enum tokentype type;
	uint32_t line;
	uint32_t column;
	int value;
};

static int has_ul_tail(const char *p) {
	char tail[4];
	int tc = 0, c;
	while(tc < 4 ) {
		if(!*p) break;
		c = tolower(*p);
		if(c == 'u' || c == 'l') {
			tail[tc++] = c;
		} else {
			return 0;
		}
		p++;
	}
	if(tc == 1) return 1;
	if(tc == 2) {
		if(!memcmp(tail, "lu", 2)) return 1;
		if(!memcmp(tail, "ul", 2)) return 1;
		if(!memcmp(tail, "ll", 2)) return 1;
	}
	if(tc == 3) {
		if(!memcmp(tail, "llu", 3)) return 1;
		if(!memcmp(tail, "ull", 3)) return 1;
	}
	return 0;
}

static int is_hex_int_literal(const char *s) {
	if(s[0] == '-') s++;
	if(s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		const char* p = s+2;
		while(*p) {
			if(!strchr("0123456789abcdef", tolower(*p))) {
				if(p == s+2) return 0;
				return has_ul_tail(p);
			}
			p++;
		}
		return 1;
	}
	return 0;
}

static int is_dec_int_literal(const char *s) {
	if(s[0] == '-') s++;
	if(s[0] == '0') return 0;
	while(*s) {
		if(!isdigit(*(s++))) {
			return has_ul_tail(s);
			return 0;
		}
	}
	return 1;
}
static int is_oct_int_literal(const char *s) {
	if(s[0] == '-') s++;
	if(s[0] != '0') return 0;
	while(*s) {
		if(!strchr("01234567", *s)) return 0;
		s++;
	}
	return 1;
}

static int is_ellipsis(const char *s) {
	return !strcmp(s, "...");
}

static int is_identifier(const char *s) {
	#define ALPHA_UP "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	#define ALPHA_LO "abcdefghijklmnopqrstuvwxyz"
	#define DIGIT    "0123456789"
	static const char iden_head[] = "_" ALPHA_UP ALPHA_LO;
	static const char iden_tail[] = "_" ALPHA_UP ALPHA_LO DIGIT;

	if(!strchr(iden_head, *(s++))) return 0;
	while(*s) {
		if(!strchr(iden_tail, *s))
			return 0;
		s++;
	}
	return 1;
}

static enum tokentype categorize(const char *s) {
	if(is_ellipsis(s)) return TT_ELLIPSIS;
	if(is_hex_int_literal(s)) return TT_HEX_INT_LIT;
	if(is_dec_int_literal(s)) return TT_DEC_INT_LIT;
	if(is_oct_int_literal(s)) return TT_OCT_INT_LIT;
	if(is_identifier(s)) return TT_IDENTIFIER;
	return TT_UNKNOWN;
}


static int is_sep(int c) {
	return !!strchr(" \t\n()[]<>{}\?:;.,!=+-*&|/%#'\"", c);
}

static int apply_coords(struct tokenizer *t, struct token* out, char *end, int retval) {
	out->line = t->line;
	uintptr_t len = end - t->buf;
	out->column = t->column - len;
	return retval;
}

static inline char *assign_bufchar(struct tokenizer *t, char *s, int c) {
	t->column++;
	*s = c;
	return s + 1;
}

static int get_string(struct tokenizer *t, struct token* out) {
	char *s = t->buf+1;
	int escaped = 0;
	while((uintptr_t)s < (uintptr_t)t->buf + MAX_TOK_LEN + 2) {
		int c = getc(t->input);
		if(c == EOF) {
			out->type = TT_EOF;
			*s = 0;
			return apply_coords(t, out, s, 0);
		}
		if(c == '\n') {
			out->type = TT_UNKNOWN;
			s = assign_bufchar(t, s, 0);
			return apply_coords(t, out, s, 0);
		}
		if(!escaped) {
			if(c == '"') {
				s = assign_bufchar(t, s, c);
				*s = 0;
				//s = assign_bufchar(t, s, 0);
				out->type = TT_STRING_LIT;
				return apply_coords(t, out, s, 1);
			}
			if(c == '\\') escaped = 1;
		} else {
			escaped = 0;
		}
		s = assign_bufchar(t, s, c);
	}
	t->buf[MAX_TOK_LEN-1] = 0;
	out->type = TT_OVERFLOW;
	return apply_coords(t, out, s, 0);
}

static int get_char(struct tokenizer *t, struct token* out) {
	char *s = t->buf+1;
	int escaped = 0, cnt = 0;
	while(cnt < 3 && (uintptr_t)s < (uintptr_t)t->buf + MAX_TOK_LEN + 2) {
		int c = getc(t->input);
		if(c == EOF) {
			out->type = TT_EOF;
			*s = 0;
			return apply_coords(t, out, s, 0);
		}
		if(cnt == 0) {
			if(c == '\\') escaped = 1;
		} else if(cnt == 1) {
			if(!escaped && c != '\'') {
wrong:
				s = assign_bufchar(t, s, c);
				out->type = TT_UNKNOWN;
				*s = 0;
				return apply_coords(t, out, s, 0);
			} else if(!escaped && c == '\'') {
right:
				out->type = TT_CHAR_LIT;
				s = assign_bufchar(t, s, c);
				*s = 0;
				return apply_coords(t, out, s, 1);
			}
		} else if(cnt == 2) {
			if(c != '\'') {
				goto wrong;
			}
			goto right;
		}
		s = assign_bufchar(t, s, c);
		cnt++;
	}
	*s = 0;
	out->type = TT_OVERFLOW;
	return apply_coords(t, out, s, 0);
}

int tokenizer_next(struct tokenizer *t, struct token* out) {
	char *s = t->buf;
	out->value = 0;
	while(1) {
		int c = getc(t->input);
		if(c == EOF) {
			out->type = TT_EOF;
			return apply_coords(t, out, s, 1);
		}
		if(is_sep(c)) {
			ungetc(c, t->input);
			break;
		}

		s = assign_bufchar(t, s, c);
		if(t->column + 1 >= MAX_TOK_LEN) {
			out->type = TT_OVERFLOW;
			return apply_coords(t, out, s, 0);
		}
	}
	if(s == t->buf) {
		int c = getc(t->input);
		s = assign_bufchar(t, s, c);
		*s = 0;
		//s = assign_bufchar(t, s, 0);
		if(c == '"') return get_string(t, out);
		if(c == '\'') return get_char(t, out);
		out->type = TT_SEP;
		out->value = c;
		if(c == '\n') {
			apply_coords(t, out, s, 1);
			t->line++;
			t->column=0;
			return 1;
		}
		return apply_coords(t, out, s, 1);
	}
	//s = assign_bufchar(t, s, 0);
	*s = 0;
	out->type = categorize(t->buf);
	return apply_coords(t, out, s, out->type != TT_UNKNOWN);
}

void tokenizer_init(struct tokenizer *t, FILE* in) {
	t->input = in;
	t->line = 1;
	t->column = 0;
}

int main(int argc, char** argv) {
	struct tokenizer t;
	struct token curr;
	tokenizer_init(&t, stdin);
	int ret;
	while((ret = tokenizer_next(&t, &curr)) && curr.type != TT_EOF) {
		dprintf(1, "(stdin:%u,%u) ", curr.line, curr.column);
		if(curr.type == TT_SEP)
			dprintf(1, "separator: %c\n", curr.value == '\n'? ' ' : curr.value);
		else
			dprintf(1, "%s: %s\n", tokentype_to_str(curr.type), t.buf);
	}
	if(!ret) {
		dprintf(2, "error occured on %u:%u\n", curr.line, curr.column);
		dprintf(2, "%s\n", t.buf);
		for(int i = 0; i < strlen(t.buf); i++)
			dprintf(2, "^");
		dprintf(2, "\n");
	}
}
