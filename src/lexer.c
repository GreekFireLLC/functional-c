#include "fc.h"

#define MAX_LITERAL_MAGNITUDE 2147483648LL

static bool is_ascii_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_ascii_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool is_ascii_alnum(char c) { return is_ascii_letter(c) || is_ascii_digit(c); }

typedef struct {
    const char *word;
    TokenKind kind;
} KeywordEntry;

static const KeywordEntry KEYWORDS[] = {
    {"int", TOK_KW_INT},
    {"bool", TOK_KW_BOOL},
    {"_Bool", TOK_KW_BOOL},
    {"char", TOK_KW_CHAR},
    {"signed", TOK_KW_SIGNED},
    {"unsigned", TOK_KW_UNSIGNED},
    {"let", TOK_KW_LET},
    {"return", TOK_KW_RETURN},
    {"true", TOK_KW_TRUE},
    {"false", TOK_KW_FALSE},
    {"if", TOK_KW_IF},
    {"else", TOK_KW_ELSE},
    {"while", TOK_KW_WHILE},
    {"for", TOK_KW_FOR},
    {"do", TOK_KW_DO},
    {"break", TOK_KW_BREAK},
    {"continue", TOK_KW_CONTINUE},
};

static const char *const RESERVED_C_WORDS[] = {
    "switch", "case", "default", "goto",
    "struct", "union", "enum", "typedef", "void", "short", "long", "float", "double",
    "const", "static", "extern", "auto", "register", "volatile",
    "inline", "restrict", "sizeof",
};

static bool lookup_keyword(const char *w, size_t len, TokenKind *out) {
    for (size_t i = 0; i < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]); i++) {
        if (strlen(KEYWORDS[i].word) == len && memcmp(KEYWORDS[i].word, w, len) == 0) {
            *out = KEYWORDS[i].kind;
            return true;
        }
    }
    return false;
}

static bool is_reserved_c_word(const char *w, size_t len) {
    for (size_t i = 0; i < sizeof(RESERVED_C_WORDS) / sizeof(RESERVED_C_WORDS[0]); i++) {
        if (strlen(RESERVED_C_WORDS[i]) == len && memcmp(RESERVED_C_WORDS[i], w, len) == 0) {
            return true;
        }
    }
    return false;
}

typedef struct {
    Arena *arena;
    const char *text;
    size_t len;
    const char *file;
    size_t pos;
    int line;
    int column;
    DiagList *diags;
    jmp_buf err_jmp;
} Lexer;

static char lx_peek(const Lexer *lx, size_t offset) {
    return (lx->pos + offset < lx->len) ? lx->text[lx->pos + offset] : '\0';
}

static SourceLoc lx_here(const Lexer *lx) {
    SourceLoc loc = {lx->file, lx->line, lx->column};
    return loc;
}

static void lx_advance(Lexer *lx) {
    if (lx->text[lx->pos] == '\n') {
        lx->line++;
        lx->column = 1;
    } else {
        lx->column++;
    }
    lx->pos++;
}

static void lx_error(Lexer *lx, const char *code, SourceLoc at, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char stack_buf[512];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
    va_end(ap);
    char *msg;
    if (n >= 0 && (size_t)n < sizeof(stack_buf)) {
        msg = arena_strdup(lx->arena, stack_buf);
    } else {
        size_t sz = (n > 0) ? (size_t)n + 1 : 1024;
        msg = arena_alloc(lx->arena, sz);
        vsnprintf(msg, sz, fmt, ap2);
    }
    va_end(ap2);
    diag_add(lx->diags, lx->arena, code, at, false, "%s", msg);
    longjmp(lx->err_jmp, 1);
}

static void tok_push(TokenList *list, Token tok) {
    if (list->count >= list->cap) {
        int new_cap = list->cap ? list->cap * 2 : 256;
        Token *next = realloc(list->items, (size_t)new_cap * sizeof(Token));
        if (!next) {
            fprintf(stderr, "out of memory\n");
            exit(3);
        }
        list->items = next;
        list->cap = new_cap;
    }
    list->items[list->count++] = tok;
}

static void lx_skip_ws_and_comments(Lexer *lx) {
    while (lx->pos < lx->len) {
        char c = lx_peek(lx, 0);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            lx_advance(lx);
        } else if (c == '/' && lx_peek(lx, 1) == '/') {
            while (lx->pos < lx->len && lx_peek(lx, 0) != '\n') {
                lx_advance(lx);
            }
        } else if (c == '/' && lx_peek(lx, 1) == '*') {
            SourceLoc start = lx_here(lx);
            lx_advance(lx);
            lx_advance(lx);
            for (;;) {
                if (lx->pos >= lx->len) {
                    lx_error(lx, "FC1002", start, "unterminated block comment: '/*' has no matching '*/'");
                }
                if (lx_peek(lx, 0) == '*' && lx_peek(lx, 1) == '/') {
                    lx_advance(lx);
                    lx_advance(lx);
                    break;
                }
                lx_advance(lx);
            }
        } else {
            return;
        }
    }
}

static Token lx_number(Lexer *lx) {
    SourceLoc start = lx_here(lx);
    size_t begin = lx->pos;
    while (is_ascii_digit(lx_peek(lx, 0))) {
        lx_advance(lx);
    }
    size_t dlen = lx->pos - begin;
    char *digits = arena_strndup(lx->arena, lx->text + begin, dlen);

    char next = lx_peek(lx, 0);
    if (is_ascii_letter(next) || next == '_') {
        if (dlen == 1 && digits[0] == '0' && (next == 'x' || next == 'X')) {
            lx_error(lx, "FC1004", start, "hexadecimal literals are not supported in version 0; use decimal");
        }
        size_t suffix_start = lx->pos;
        while (is_ascii_alnum(lx_peek(lx, 0)) || lx_peek(lx, 0) == '_') {
            lx_advance(lx);
        }
        char *full = arena_strndup(lx->arena, lx->text + begin, lx->pos - begin);
        char *suffix = arena_strndup(lx->arena, lx->text + suffix_start, lx->pos - suffix_start);
        lx_error(lx, "FC1004", start,
            "invalid integer literal '%s': only plain decimal integers are supported (no suffixes such as '%s')",
            full, suffix);
    }

    if (next == '.') {
        lx_error(lx, "FC1004", start,
            "floating-point literals are not supported yet; the number types are 'int', 'char' and 'unsigned char'");
    }

    if (dlen > 1 && digits[0] == '0') {
        lx_error(lx, "FC1004", start,
            "integer literal '%s' has a leading zero; C would read it as octal, so it is rejected (write it without leading zeros)",
            digits);
    }

    int64_t val = 0;
    if (dlen > 10) {
        lx_error(lx, "FC1003", start,
            "integer literal %s is out of range for a 32-bit int (-2147483648 to 2147483647)", digits);
    }
    for (size_t i = 0; i < dlen; i++) {
        val = val * 10 + (digits[i] - '0');
    }
    if (val > MAX_LITERAL_MAGNITUDE) {
        lx_error(lx, "FC1003", start,
            "integer literal %s is out of range for a 32-bit int (-2147483648 to 2147483647)", digits);
    }

    Token t = {TOK_INT_LIT, digits, (int)dlen, start, val};
    return t;
}

static Token lx_character(Lexer *lx) {
    SourceLoc start = lx_here(lx);
    size_t begin = lx->pos;
    lx_advance(lx); /* opening quote */
    char c = lx_peek(lx, 0);
    int64_t value = 0;

    if (c == '\0' || c == '\n' || c == '\r') {
        lx_error(lx, "FC1004", start, "unterminated character literal: expected a character and a closing quote");
    }
    if (c == '\'') {
        lx_error(lx, "FC1004", start, "empty character literal; write one character between the quotes, as in 'a'");
    }

    if (c == '\\') {
        lx_advance(lx);
        char esc = lx_peek(lx, 0);
        switch (esc) {
            case 'n': value = 10; break;
            case 't': value = 9; break;
            case 'r': value = 13; break;
            case '0': value = 0; break;
            case 'a': value = 7; break;
            case 'b': value = 8; break;
            case 'f': value = 12; break;
            case 'v': value = 11; break;
            case '\\': value = 92; break;
            case '\'': value = 39; break;
            case '"': value = 34; break;
            case '?': value = 63; break;
            default:
                lx_error(lx, "FC1004", start,
                    "unsupported escape sequence in a character literal; the supported ones are \\n \\t \\r \\0 \\a \\b \\f \\v \\\\ \\' \\\" and \\?");
        }
        if (esc == '0' && is_ascii_digit(lx_peek(lx, 1))) {
            lx_error(lx, "FC1004", start, "octal escape sequences are not supported in a character literal; only '\\0' is");
        }
    } else if ((unsigned char)c < ' ' || (unsigned char)c > '~') {
        lx_error(lx, "FC1004", start, "only printable ASCII characters and escape sequences are supported in a character literal");
    } else {
        value = (unsigned char)c;
    }

    lx_advance(lx);
    if (lx_peek(lx, 0) != '\'') {
        lx_error(lx, "FC1004", start,
            "a character literal holds exactly one character and must end with a quote; multi-character literals and strings are not supported");
    }
    lx_advance(lx); /* closing quote */

    size_t tlen = lx->pos - begin;
    char *raw = arena_strndup(lx->arena, lx->text + begin, tlen);
    Token t = {TOK_INT_LIT, raw, (int)tlen, start, value};
    return t;
}

static Token lx_word(Lexer *lx) {
    SourceLoc start = lx_here(lx);
    size_t begin = lx->pos;
    while (is_ascii_alnum(lx_peek(lx, 0)) || lx_peek(lx, 0) == '_') {
        lx_advance(lx);
    }
    size_t wlen = lx->pos - begin;
    char *word = arena_strndup(lx->arena, lx->text + begin, wlen);
    TokenKind kw;
    if (lookup_keyword(word, wlen, &kw)) {
        Token t = {kw, word, (int)wlen, start, 0};
        return t;
    }
    TokenKind kind = is_reserved_c_word(word, wlen) ? TOK_RESERVED_WORD : TOK_IDENT;
    Token t = {kind, word, (int)wlen, start, 0};
    return t;
}

static Token lx_take(Lexer *lx, TokenKind kind, const char *text, int len, SourceLoc start) {
    for (int i = 0; i < len; i++) {
        lx_advance(lx);
    }
    Token t = {kind, text, len, start, 0};
    return t;
}

static Token lx_operator(Lexer *lx) {
    SourceLoc start = lx_here(lx);
    char c = lx_peek(lx, 0);
    char n = lx_peek(lx, 1);
    char p2 = lx_peek(lx, 2);

    if ((c == '<' && n == '<' && p2 == '=') || (c == '>' && n == '>' && p2 == '=')) {
        char s[4] = {c, n, p2, '\0'};
        lx_error(lx, "FC1005", start, "compound assignment '%s' is not supported: there are no shift operators yet", s);
    }

    if (c == '<' && n == '=') return lx_take(lx, TOK_LE, "<=", 2, start);
    if (c == '>' && n == '=') return lx_take(lx, TOK_GE, ">=", 2, start);
    if (c == '=' && n == '=') return lx_take(lx, TOK_EQEQ, "==", 2, start);
    if (c == '!' && n == '=') return lx_take(lx, TOK_BANGEQ, "!=", 2, start);
    if (c == '&' && n == '&') return lx_take(lx, TOK_AMPAMP, "&&", 2, start);
    if (c == '|' && n == '|') return lx_take(lx, TOK_PIPEPIPE, "||", 2, start);
    if (c == '=' && n == '>') return lx_take(lx, TOK_ARROW, "=>", 2, start);
    if (c == '-' && n == '>') {
        lx_error(lx, "FC1005", start, "'->' member access is not supported: version 0 has no pointers or structures");
    }
    if (c == '+' && n == '+') return lx_take(lx, TOK_PLUSPLUS, "++", 2, start);
    if (c == '-' && n == '-') return lx_take(lx, TOK_MINUSMINUS, "--", 2, start);
    if (c == '+' && n == '=') return lx_take(lx, TOK_PLUS_ASSIGN, "+=", 2, start);
    if (c == '-' && n == '=') return lx_take(lx, TOK_MINUS_ASSIGN, "-=", 2, start);
    if (c == '*' && n == '=') return lx_take(lx, TOK_STAR_ASSIGN, "*=", 2, start);
    if (c == '/' && n == '=') return lx_take(lx, TOK_SLASH_ASSIGN, "/=", 2, start);
    if (c == '%' && n == '=') return lx_take(lx, TOK_PERCENT_ASSIGN, "%=", 2, start);
    if ((c == '&' || c == '|' || c == '^') && n == '=') {
        char s[3] = {c, n, '\0'};
        lx_error(lx, "FC1005", start, "compound assignment '%s' is not supported: there are no bitwise operators yet", s);
    }
    if ((c == '<' && n == '<') || (c == '>' && n == '>')) {
        char s[3] = {c, n, '\0'};
        lx_error(lx, "FC1005", start, "shift operator '%s' is not supported in version 0", s);
    }

    switch (c) {
        case '(': return lx_take(lx, TOK_LPAREN, "(", 1, start);
        case ')': return lx_take(lx, TOK_RPAREN, ")", 1, start);
        case '{': return lx_take(lx, TOK_LBRACE, "{", 1, start);
        case '}': return lx_take(lx, TOK_RBRACE, "}", 1, start);
        case ',': return lx_take(lx, TOK_COMMA, ",", 1, start);
        case ';': return lx_take(lx, TOK_SEMICOLON, ";", 1, start);
        case '?': return lx_take(lx, TOK_QUESTION, "?", 1, start);
        case ':': return lx_take(lx, TOK_COLON, ":", 1, start);
        case '=': return lx_take(lx, TOK_ASSIGN, "=", 1, start);
        case '+': return lx_take(lx, TOK_PLUS, "+", 1, start);
        case '-': return lx_take(lx, TOK_MINUS, "-", 1, start);
        case '*': return lx_take(lx, TOK_STAR, "*", 1, start);
        case '/': return lx_take(lx, TOK_SLASH, "/", 1, start);
        case '%': return lx_take(lx, TOK_PERCENT, "%", 1, start);
        case '<': return lx_take(lx, TOK_LT, "<", 1, start);
        case '>': return lx_take(lx, TOK_GT, ">", 1, start);
        case '!': return lx_take(lx, TOK_BANG, "!", 1, start);
        case '#':
            lx_error(lx, "FC1005", start, "preprocessor directives ('#') are not supported: there are no headers or macros in version 0");
            break;
        case '[':
        case ']':
            lx_error(lx, "FC1005", start, "arrays and indexing ('[' ']') are not supported in version 0");
            break;
        case '&':
            lx_error(lx, "FC1005", start, "'&' (address-of / bitwise and) is not supported; use '&&' for logical and");
            break;
        case '|':
            lx_error(lx, "FC1005", start, "bitwise '|' is not supported; use '||' for logical or");
            break;
        case '^':
        case '~':
            lx_error(lx, "FC1005", start, "bitwise operator '%c' is not supported in version 0", c);
            break;
        case '.':
            lx_error(lx, "FC1005", start, "'.' is not supported: version 0 has no structures or floating-point numbers");
            break;
        case '"':
            lx_error(lx, "FC1005", start, "string literals are not supported in version 0");
            break;
        default:
            break;
    }

    unsigned char uc = (unsigned char)c;
    if (uc < 0x20 || uc > 0x7E) {
        lx_error(lx, "FC1001", start, "unexpected character U+%04X", (unsigned int)uc);
    } else {
        lx_error(lx, "FC1001", start, "unexpected character '%c'", c);
    }
    Token eof = {TOK_EOF, "", 0, start, 0};
    return eof;
}

bool fc_lex(Arena *a, const char *text, const char *file, TokenList *out_tokens, DiagList *diags) {
    out_tokens->items = NULL;
    out_tokens->count = 0;
    out_tokens->cap = 0;

    Lexer lx;
    lx.arena = a;
    lx.text = text;
    lx.len = strlen(text);
    lx.file = file;
    lx.pos = 0;
    lx.line = 1;
    lx.column = 1;
    lx.diags = diags;

    if (setjmp(lx.err_jmp) != 0) {
        free(out_tokens->items);
        out_tokens->items = NULL;
        out_tokens->count = 0;
        out_tokens->cap = 0;
        return false;
    }

    for (;;) {
        lx_skip_ws_and_comments(&lx);
        if (lx.pos >= lx.len) {
            Token eof = {TOK_EOF, "", 0, lx_here(&lx), 0};
            tok_push(out_tokens, eof);
            return true;
        }
        char c = lx_peek(&lx, 0);
        if (is_ascii_digit(c)) {
            tok_push(out_tokens, lx_number(&lx));
        } else if (c == '\'') {
            tok_push(out_tokens, lx_character(&lx));
        } else if (is_ascii_letter(c) || c == '_') {
            tok_push(out_tokens, lx_word(&lx));
        } else {
            tok_push(out_tokens, lx_operator(&lx));
        }
    }
}
