#ifndef FC_H
#define FC_H

#include <inttypes.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- arena */

typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t used;
    size_t cap;
    unsigned char data[];
} ArenaBlock;

typedef struct {
    ArenaBlock *head;
} Arena;

void arena_init(Arena *a);
void *arena_alloc(Arena *a, size_t size);
char *arena_strndup(Arena *a, const char *s, size_t len);
char *arena_strdup(Arena *a, const char *s);
char *arena_printf(Arena *a, const char *fmt, ...);
void arena_free(Arena *a);

/* ---------------------------------------------------------------- dynamic string buffer */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

void sb_init(StrBuf *sb);
void sb_free(StrBuf *sb);
void sb_reserve(StrBuf *sb, size_t extra);
void sb_append_n(StrBuf *sb, const char *s, size_t n);
void sb_append(StrBuf *sb, const char *s);
void sb_append_char(StrBuf *sb, char c);
void sb_printf(StrBuf *sb, const char *fmt, ...);
void sb_line(StrBuf *sb, const char *s);

/* ---------------------------------------------------------------- source locations & diagnostics */

typedef struct {
    const char *file;
    int line;
    int column;
} SourceLoc;

typedef struct {
    const char *code;
    const char *message;
    SourceLoc loc;
    bool is_warning;
    int order; /* insertion order for stable sort */
} Diagnostic;

typedef struct {
    Diagnostic *items;
    int count;
    int cap;
} DiagList;

void diag_init(DiagList *list);
void diag_add(DiagList *list, Arena *a, const char *code, SourceLoc loc, bool is_warning, const char *fmt, ...);
void diag_sort(DiagList *list);
bool diag_has_errors(const DiagList *list);
int diag_error_count(const DiagList *list);
void diag_print(const Diagnostic *d, FILE *out);
void diag_free(DiagList *list);

/* ---------------------------------------------------------------- types & operators */

typedef enum {
    FC_TYPE_NONE = 0, /* error / unknown */
    FC_TYPE_INT,
    FC_TYPE_BOOL,
    FC_TYPE_CHAR,
    FC_TYPE_UCHAR
} FcType;

const char *fc_type_name(FcType t);

typedef enum {
    UNOP_NEGATE,
    UNOP_NOT
} UnaryOp;

typedef enum {
    BINOP_ADD,
    BINOP_SUB,
    BINOP_MUL,
    BINOP_DIV,
    BINOP_REM,
    BINOP_LT,
    BINOP_LE,
    BINOP_GT,
    BINOP_GE,
    BINOP_EQ,
    BINOP_NE,
    BINOP_AND,
    BINOP_OR
} BinaryOp;

const char *binop_source(BinaryOp op);

typedef enum {
    ASSIGN_SET,
    ASSIGN_ADD,
    ASSIGN_SUB,
    ASSIGN_MUL,
    ASSIGN_DIV,
    ASSIGN_REM,
    ASSIGN_INC,
    ASSIGN_DEC
} AssignOp;

const char *assignop_source(AssignOp op);

/* ---------------------------------------------------------------- tokens */

typedef enum {
    TOK_IDENT,
    TOK_INT_LIT,
    TOK_RESERVED_WORD,

    TOK_KW_INT,
    TOK_KW_BOOL,
    TOK_KW_CHAR,
    TOK_KW_SIGNED,
    TOK_KW_UNSIGNED,
    TOK_KW_LET,
    TOK_KW_RETURN,
    TOK_KW_TRUE,
    TOK_KW_FALSE,
    TOK_KW_IF,
    TOK_KW_ELSE,
    TOK_KW_WHILE,
    TOK_KW_FOR,
    TOK_KW_DO,
    TOK_KW_BREAK,
    TOK_KW_CONTINUE,

    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_COMMA,
    TOK_SEMICOLON,
    TOK_QUESTION,
    TOK_COLON,
    TOK_ARROW,
    TOK_ASSIGN,

    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_LT,
    TOK_LE,
    TOK_GT,
    TOK_GE,
    TOK_EQEQ,
    TOK_BANGEQ,
    TOK_AMPAMP,
    TOK_PIPEPIPE,
    TOK_BANG,

    TOK_PLUSPLUS,
    TOK_MINUSMINUS,
    TOK_PLUS_ASSIGN,
    TOK_MINUS_ASSIGN,
    TOK_STAR_ASSIGN,
    TOK_SLASH_ASSIGN,
    TOK_PERCENT_ASSIGN,

    TOK_EOF
} TokenKind;

typedef struct {
    TokenKind kind;
    const char *text;
    int text_len;
    SourceLoc loc;
    int64_t int_value;
} Token;

typedef struct {
    Token *items;
    int count;
    int cap;
} TokenList;

/* ---------------------------------------------------------------- symbols */

typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct FunctionDecl FunctionDecl;

typedef enum {
    SYM_FUNCTION,
    SYM_LAMBDA,
    SYM_VALUE
} SymbolKind;

typedef struct Symbol {
    SymbolKind kind;
    const char *name;
    const char *c_name;
    SourceLoc declared_at;

    /* For SYM_FUNCTION */
    FunctionDecl *func_decl;
    bool is_recursive;

    /* For SYM_FUNCTION and SYM_LAMBDA */
    FcType *param_types;
    int param_count;
    FcType return_type;

    /* For SYM_VALUE */
    FcType value_type;
    const char *value_kind; /* "parameter", "variable", "let binding", "lambda parameter" */
    bool is_mutable;
    bool is_assigned;
} Symbol;

/* ---------------------------------------------------------------- AST */

typedef struct {
    FcType type;
    const char *name;
    SourceLoc loc;
    Symbol *symbol;
} Parameter;

typedef enum {
    EXPR_INT_LIT,
    EXPR_BOOL_LIT,
    EXPR_NAME,
    EXPR_UNARY,
    EXPR_CAST,
    EXPR_BINARY,
    EXPR_COND,
    EXPR_CALL,
    EXPR_LAMBDA
} ExprKind;

struct Expr {
    ExprKind kind;
    SourceLoc loc;
    FcType type;
    FcType convert_to; /* FC_TYPE_NONE if no implicit conversion */

    union {
        int64_t int_val;
        bool bool_val;
        struct {
            const char *name;
            Symbol *symbol;
        } name;
        struct {
            UnaryOp op;
            Expr *operand;
        } unary;
        struct {
            FcType target;
            Expr *operand;
        } cast;
        struct {
            BinaryOp op;
            Expr *left;
            Expr *right;
            SourceLoc op_loc;
        } binary;
        struct {
            Expr *cond;
            Expr *when_true;
            Expr *when_false;
            SourceLoc q_loc;
        } cond;
        struct {
            const char *callee;
            Expr **args;
            int arg_count;
            Symbol *symbol;
            bool is_print;
        } call;
        struct {
            Parameter *params;
            int param_count;
            Expr *body;
        } lambda;
    } u;
};

typedef struct {
    const char *name;
    SourceLoc name_loc;
    Expr *init;
    bool has_initializer;
    Symbol *symbol;
} VarDeclarator;

typedef enum {
    STMT_EMPTY,
    STMT_BLOCK,
    STMT_LET,
    STMT_DECL,
    STMT_ASSIGN,
    STMT_EXPR,
    STMT_RETURN,
    STMT_IF,
    STMT_WHILE,
    STMT_DO_WHILE,
    STMT_FOR,
    STMT_BREAK,
    STMT_CONTINUE
} StmtKind;

struct Stmt {
    StmtKind kind;
    SourceLoc loc;
    bool can_complete_normally;
    /* Loop flow flags (for WHILE, DO_WHILE, FOR) */
    bool uses_break;
    bool uses_continue;

    union {
        struct {
            Stmt **stmts;
            int count;
        } block;
        struct {
            const char *name;
            SourceLoc name_loc;
            Expr *init;
            Symbol *symbol;
        } let_s;
        struct {
            FcType type;
            VarDeclarator *decls;
            int count;
        } decl;
        struct {
            const char *name;
            AssignOp op;
            Expr *value; /* NULL for ++/-- */
            Symbol *symbol;
        } assign;
        struct {
            Expr *expr;
        } expr_s;
        struct {
            Expr *value;
        } ret;
        struct {
            Expr *cond;
            Stmt *then_branch;
            Stmt *else_branch;
        } if_s;
        struct {
            Expr *cond;
            Stmt *body;
        } while_s;
        struct {
            Stmt *body;
            Expr *cond;
        } do_s;
        struct {
            Stmt **init;
            int init_count;
            Expr *cond; /* NULL if omitted */
            Stmt **update;
            int update_count;
            Stmt *body;
        } for_s;
    } u;
};

struct FunctionDecl {
    FcType return_type;
    SourceLoc return_type_loc;
    const char *name;
    SourceLoc name_loc;
    Parameter *params;
    int param_count;
    Stmt **body;
    int body_count;
    SourceLoc close_brace_loc;
    Symbol *symbol;
};

typedef struct {
    const char *file;
    FunctionDecl **functions;
    int func_count;
} ProgramNode;

/* ---------------------------------------------------------------- pipeline entry points */

bool fc_lex(Arena *a, const char *text, const char *file, TokenList *out_tokens, DiagList *diags);
ProgramNode *fc_parse(Arena *a, const TokenList *tokens, const char *file, DiagList *diags);
void fc_check(Arena *a, ProgramNode *program, bool strict, DiagList *diags);
void fc_emit_c(Arena *a, const ProgramNode *program, const char *source_name, StrBuf *out);

/* Shared flow helpers */
bool fc_is_always_true(const Expr *cond);

#endif /* FC_H */
