#include "fc.h"

typedef struct {
    Arena *arena;
    const Token *tokens;
    int count;
    int index;
    const char *file;
    DiagList *diags;
    jmp_buf err_jmp;
} Parser;

static const Token *p_cur(const Parser *p) { return &p->tokens[p->index]; }

static const Token *p_prev(const Parser *p) {
    return &p->tokens[p->index > 0 ? p->index - 1 : 0];
}

static const Token *p_peek(const Parser *p, int offset) {
    int idx = p->index + offset;
    if (idx >= p->count) idx = p->count - 1;
    return &p->tokens[idx];
}

static bool p_check(const Parser *p, TokenKind kind) { return p_cur(p)->kind == kind; }

static const Token *p_advance(Parser *p) {
    const Token *t = p_cur(p);
    if (t->kind != TOK_EOF) {
        p->index++;
    }
    return t;
}

static bool p_match(Parser *p, TokenKind kind) {
    if (!p_check(p, kind)) return false;
    p_advance(p);
    return true;
}

static SourceLoc tok_end(const Token *t) {
    SourceLoc loc = t->loc;
    loc.column += t->text_len;
    return loc;
}

static const char *tok_describe(Arena *a, const Token *t) {
    if (t->kind == TOK_EOF) return "end of file";
    return arena_printf(a, "'%s'", t->text);
}

static void p_error(Parser *p, const char *code, SourceLoc at, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char stack_buf[512];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
    va_end(ap);
    char *msg;
    if (n >= 0 && (size_t)n < sizeof(stack_buf)) {
        msg = arena_strdup(p->arena, stack_buf);
    } else {
        size_t sz = (n > 0) ? (size_t)n + 1 : 1024;
        msg = arena_alloc(p->arena, sz);
        vsnprintf(msg, sz, fmt, ap2);
    }
    va_end(ap2);
    diag_add(p->diags, p->arena, code, at, false, "%s", msg);
    longjmp(p->err_jmp, 1);
}

static void p_unsupported(Parser *p, const Token *tok) {
    const char *w = tok->text;
    if (strcmp(w, "switch") == 0 || strcmp(w, "case") == 0 || strcmp(w, "default") == 0) {
        p_error(p, "FC2002", tok->loc,
            "'%s' is not supported yet; use 'if (...) { ... } else if (...) { ... } else { ... }'", w);
    } else if (strcmp(w, "goto") == 0) {
        p_error(p, "FC2002", tok->loc, "'goto' is not supported; use loops with 'break' and 'continue'");
    } else if (strcmp(w, "struct") == 0 || strcmp(w, "union") == 0 || strcmp(w, "enum") == 0 || strcmp(w, "typedef") == 0) {
        p_error(p, "FC2002", tok->loc, "'%s' declarations are not supported in version 0", w);
    } else if (strcmp(w, "void") == 0) {
        p_error(p, "FC2002", tok->loc,
            "'void' is not supported yet: every function returns a value ('int', 'bool', 'char' or 'unsigned char')");
    } else if (strcmp(w, "short") == 0 || strcmp(w, "long") == 0 || strcmp(w, "float") == 0 || strcmp(w, "double") == 0) {
        p_error(p, "FC2002", tok->loc,
            "type '%s' is not supported yet: the available types are 'int', 'bool', 'char' and 'unsigned char'", w);
    } else if (strcmp(w, "sizeof") == 0) {
        p_error(p, "FC2002", tok->loc, "'sizeof' is not supported in version 0");
    } else {
        p_error(p, "FC2002", tok->loc, "'%s' is not supported in version 0", w);
    }
}

static void p_pointer_error(Parser *p, const Token *star) {
    p_error(p, "FC2002", star->loc, "pointers are not supported in version 0");
}

static void p_increment_in_expr(Parser *p, const Token *tok) {
    p_error(p, "FC2002", tok->loc,
        "'%s' can only be used as a statement of its own, as in 'x%s;', not inside an expression",
        tok->text, tok->text);
}

static const Token *p_expect(Parser *p, TokenKind kind, const char *what) {
    if (p_check(p, kind)) return p_advance(p);
    const Token *cur = p_cur(p);
    if (kind == TOK_IDENT && cur->kind == TOK_RESERVED_WORD) {
        p_error(p, "FC2002", cur->loc, "'%s' is a reserved C keyword and cannot be used as a name", cur->text);
    }
    if (kind == TOK_IDENT && cur->kind >= TOK_KW_INT && cur->kind <= TOK_KW_CONTINUE) {
        p_error(p, "FC2001", cur->loc,
            "expected %s but found the keyword '%s', which cannot be used as a name", what, cur->text);
    }
    p_error(p, "FC2001", cur->loc, "expected %s but found %s", what, tok_describe(p->arena, cur));
    return cur;
}

static void p_expect_semi(Parser *p) {
    if (p_match(p, TOK_SEMICOLON)) return;
    p_error(p, "FC2001", tok_end(p_prev(p)),
        "expected ';' at the end of the statement but found %s", tok_describe(p->arena, p_cur(p)));
}

static bool is_type_start(TokenKind k) {
    return k == TOK_KW_INT || k == TOK_KW_BOOL || k == TOK_KW_CHAR || k == TOK_KW_SIGNED || k == TOK_KW_UNSIGNED;
}

static int type_length(const Parser *p, int offset) {
    TokenKind k = p_peek(p, offset)->kind;
    if (k == TOK_KW_SIGNED || k == TOK_KW_UNSIGNED) {
        TokenKind next = p_peek(p, offset + 1)->kind;
        return (next == TOK_KW_INT || next == TOK_KW_CHAR) ? 2 : 1;
    }
    return is_type_start(k) ? 1 : 0;
}

static FcType parse_type(Parser *p, const char *context) {
    const Token *tok = p_cur(p);
    switch (tok->kind) {
        case TOK_KW_INT:
            p_advance(p);
            return FC_TYPE_INT;
        case TOK_KW_BOOL:
            p_advance(p);
            return FC_TYPE_BOOL;
        case TOK_KW_CHAR:
            p_advance(p);
            return FC_TYPE_CHAR;
        case TOK_KW_SIGNED:
            p_advance(p);
            if (p_match(p, TOK_KW_CHAR)) return FC_TYPE_CHAR;
            p_match(p, TOK_KW_INT);
            return FC_TYPE_INT;
        case TOK_KW_UNSIGNED:
            p_advance(p);
            if (p_match(p, TOK_KW_CHAR)) return FC_TYPE_UCHAR;
            p_error(p, "FC2002", tok->loc,
                "'unsigned int' is not supported yet: the only unsigned type is 'unsigned char'");
            return FC_TYPE_INT;
        case TOK_RESERVED_WORD:
            p_unsupported(p, tok);
            return FC_TYPE_INT;
        default:
            p_error(p, "FC2001", tok->loc,
                "expected a type ('int', 'bool', 'char' or 'unsigned char') for %s but found %s",
                context, tok_describe(p->arena, tok));
            return FC_TYPE_INT;
    }
}

/* Forward declarations */
static Stmt *parse_stmt(Parser *p, const char *fn_name, const Token *fn_open);
static Expr *parse_expr(Parser *p);
static Expr *parse_unary(Parser *p);

/* Helpers to allocate AST nodes */
static Expr *new_expr(Parser *p, ExprKind kind, SourceLoc loc) {
    Expr *e = arena_alloc(p->arena, sizeof(Expr));
    memset(e, 0, sizeof(Expr));
    e->kind = kind;
    e->loc = loc;
    return e;
}

static Stmt *new_stmt(Parser *p, StmtKind kind, SourceLoc loc) {
    Stmt *s = arena_alloc(p->arena, sizeof(Stmt));
    memset(s, 0, sizeof(Stmt));
    s->kind = kind;
    s->loc = loc;
    s->can_complete_normally = true;
    return s;
}

/* ---------------------------------------------------------------- expressions */

static Expr *parse_cast(Parser *p) {
    const Token *open = p_advance(p); /* '(' */
    FcType target = parse_type(p, "a cast");
    p_expect(p, TOK_RPAREN, "')' to close the cast");
    Expr *e = new_expr(p, EXPR_CAST, open->loc);
    e->u.cast.target = target;
    e->u.cast.operand = parse_unary(p);
    return e;
}

static Expr *parse_lambda(Parser *p) {
    const Token *open = p_advance(p); /* '(' */
    Parameter *params = NULL;
    int param_count = 0, param_cap = 0;

    if (!p_check(p, TOK_RPAREN)) {
        do {
            FcType t = parse_type(p, "a lambda parameter");
            if (p_check(p, TOK_STAR)) p_pointer_error(p, p_cur(p));
            const Token *name = p_expect(p, TOK_IDENT, "a lambda parameter name");
            if (param_count >= param_cap) {
                int nc = param_cap ? param_cap * 2 : 4;
                Parameter *np = arena_alloc(p->arena, (size_t)nc * sizeof(Parameter));
                if (params) memcpy(np, params, (size_t)param_count * sizeof(Parameter));
                params = np;
                param_cap = nc;
            }
            params[param_count].type = t;
            params[param_count].name = name->text;
            params[param_count].loc = name->loc;
            params[param_count].symbol = NULL;
            param_count++;
        } while (p_match(p, TOK_COMMA));
    }

    p_expect(p, TOK_RPAREN, "')' to close the lambda parameter list");
    p_expect(p, TOK_ARROW, "'=>' after the lambda parameter list");
    if (p_check(p, TOK_LBRACE)) {
        p_error(p, "FC2002", p_cur(p)->loc,
            "lambda block bodies '{ ... }' are not supported in version 0; use an expression body");
    }

    Expr *e = new_expr(p, EXPR_LAMBDA, open->loc);
    e->u.lambda.params = params;
    e->u.lambda.param_count = param_count;
    e->u.lambda.body = parse_expr(p);
    return e;
}

static Expr *parse_paren_or_lambda(Parser *p) {
    const Token *open = p_cur(p);
    const Token *next = p_peek(p, 1);

    if (is_type_start(next->kind)) {
        int tlen = type_length(p, 1);
        bool is_cast = p_peek(p, 1 + tlen)->kind == TOK_RPAREN && p_peek(p, 2 + tlen)->kind != TOK_ARROW;
        return is_cast ? parse_cast(p) : parse_lambda(p);
    }
    if (next->kind == TOK_RPAREN) {
        if (p_peek(p, 2)->kind == TOK_ARROW) {
            return parse_lambda(p);
        }
        p_error(p, "FC2001", open->loc, "empty parentheses '()' are not an expression");
    }
    if (next->kind == TOK_RESERVED_WORD) {
        p_unsupported(p, next);
    }

    p_advance(p); /* '(' */
    Expr *inner = parse_expr(p);
    if (inner->kind == EXPR_NAME && p_check(p, TOK_COMMA)) {
        p_error(p, "FC2001", open->loc,
            "lambda parameters must have explicit types, for example '(int x, int y) => x + y'");
    }
    p_expect(p, TOK_RPAREN, "')' to close the parenthesized expression");
    if (p_check(p, TOK_ARROW)) {
        p_error(p, "FC2001", open->loc,
            "lambda parameters must have explicit types, for example '(int x) => x + 1'");
    }
    return inner;
}

static Expr *parse_primary(Parser *p) {
    const Token *tok = p_cur(p);
    switch (tok->kind) {
        case TOK_INT_LIT:
            p_advance(p);
            if (tok->int_value > INT32_MAX) {
                p_error(p, "FC1003", tok->loc,
                    "integer literal %s is out of range for a 32-bit int (maximum 2147483647); -2147483648 may only be written with a leading '-' directly before the digits",
                    tok->text);
            }
            {
                Expr *e = new_expr(p, EXPR_INT_LIT, tok->loc);
                e->u.int_val = tok->int_value;
                return e;
            }
        case TOK_KW_TRUE:
            p_advance(p);
            {
                Expr *e = new_expr(p, EXPR_BOOL_LIT, tok->loc);
                e->u.bool_val = true;
                return e;
            }
        case TOK_KW_FALSE:
            p_advance(p);
            {
                Expr *e = new_expr(p, EXPR_BOOL_LIT, tok->loc);
                e->u.bool_val = false;
                return e;
            }
        case TOK_IDENT:
            p_advance(p);
            {
                Expr *e = new_expr(p, EXPR_NAME, tok->loc);
                e->u.name.name = tok->text;
                return e;
            }
        case TOK_LPAREN:
            return parse_paren_or_lambda(p);
        case TOK_RESERVED_WORD:
            p_unsupported(p, tok);
            return NULL;
        case TOK_EOF:
            p_error(p, "FC2001", tok->loc, "expected an expression but reached the end of the file");
            return NULL;
        default:
            p_error(p, "FC2001", tok->loc, "expected an expression but found %s", tok_describe(p->arena, tok));
            return NULL;
    }
}

static Expr *parse_postfix_tail(Parser *p, Expr *expr) {
    while (p_check(p, TOK_LPAREN)) {
        const Token *paren = p_cur(p);
        if (expr->kind == EXPR_NAME) {
            p_advance(p);
            Expr **args = NULL;
            int arg_count = 0, arg_cap = 0;
            if (!p_check(p, TOK_RPAREN)) {
                do {
                    Expr *arg = parse_expr(p);
                    if (arg_count >= arg_cap) {
                        int nc = arg_cap ? arg_cap * 2 : 4;
                        Expr **na = arena_alloc(p->arena, (size_t)nc * sizeof(Expr *));
                        if (args) memcpy(na, args, (size_t)arg_count * sizeof(Expr *));
                        args = na;
                        arg_cap = nc;
                    }
                    args[arg_count++] = arg;
                } while (p_match(p, TOK_COMMA));
            }
            p_expect(p, TOK_RPAREN, arena_printf(p->arena, "')' to close the argument list of '%s'", expr->u.name.name));
            Expr *call = new_expr(p, EXPR_CALL, expr->loc);
            call->u.call.callee = expr->u.name.name;
            call->u.call.args = args;
            call->u.call.arg_count = arg_count;
            expr = call;
        } else if (expr->kind == EXPR_CALL) {
            p_error(p, "FC2002", paren->loc, "calling the result of a call is not supported in version 0");
        } else {
            p_error(p, "FC2002", paren->loc,
                "only named functions and let-bound lambdas can be called, as in 'name(arguments)'");
        }
    }
    if (p_check(p, TOK_PLUSPLUS) || p_check(p, TOK_MINUSMINUS)) {
        p_increment_in_expr(p, p_cur(p));
    }
    return expr;
}

static Expr *parse_unary(Parser *p) {
    const Token *tok = p_cur(p);
    switch (tok->kind) {
        case TOK_MINUS:
            p_advance(p);
            if (p_check(p, TOK_INT_LIT)) {
                const Token *lit = p_advance(p);
                int64_t val = -lit->int_value;
                if (val < INT32_MIN) {
                    p_error(p, "FC1003", tok->loc,
                        "integer literal -%s is out of range for a 32-bit int (-2147483648 to 2147483647)",
                        lit->text);
                }
                Expr *e = new_expr(p, EXPR_INT_LIT, tok->loc);
                e->u.int_val = val;
                return parse_postfix_tail(p, e);
            }
            {
                Expr *e = new_expr(p, EXPR_UNARY, tok->loc);
                e->u.unary.op = UNOP_NEGATE;
                e->u.unary.operand = parse_unary(p);
                return e;
            }
        case TOK_BANG:
            p_advance(p);
            {
                Expr *e = new_expr(p, EXPR_UNARY, tok->loc);
                e->u.unary.op = UNOP_NOT;
                e->u.unary.operand = parse_unary(p);
                return e;
            }
        case TOK_PLUSPLUS:
        case TOK_MINUSMINUS:
            p_increment_in_expr(p, tok);
            return NULL;
        case TOK_PLUS:
            p_error(p, "FC2002", tok->loc, "unary '+' is not supported in version 0");
            return NULL;
        case TOK_STAR:
            p_error(p, "FC2002", tok->loc, "pointer dereference '*' is not supported in version 0");
            return NULL;
        default:
            return parse_postfix_tail(p, parse_primary(p));
    }
}

static bool match_binary_at(int level, TokenKind kind, BinaryOp *out_op) {
    switch (level) {
        case 0:
            if (kind == TOK_PIPEPIPE) { *out_op = BINOP_OR; return true; }
            return false;
        case 1:
            if (kind == TOK_AMPAMP) { *out_op = BINOP_AND; return true; }
            return false;
        case 2:
            if (kind == TOK_EQEQ) { *out_op = BINOP_EQ; return true; }
            if (kind == TOK_BANGEQ) { *out_op = BINOP_NE; return true; }
            return false;
        case 3:
            if (kind == TOK_LT) { *out_op = BINOP_LT; return true; }
            if (kind == TOK_LE) { *out_op = BINOP_LE; return true; }
            if (kind == TOK_GT) { *out_op = BINOP_GT; return true; }
            if (kind == TOK_GE) { *out_op = BINOP_GE; return true; }
            return false;
        case 4:
            if (kind == TOK_PLUS) { *out_op = BINOP_ADD; return true; }
            if (kind == TOK_MINUS) { *out_op = BINOP_SUB; return true; }
            return false;
        case 5:
            if (kind == TOK_STAR) { *out_op = BINOP_MUL; return true; }
            if (kind == TOK_SLASH) { *out_op = BINOP_DIV; return true; }
            if (kind == TOK_PERCENT) { *out_op = BINOP_REM; return true; }
            return false;
        default:
            return false;
    }
}

static Expr *parse_binary(Parser *p, int level) {
    if (level == 6) return parse_unary(p);
    Expr *left = parse_binary(p, level + 1);
    for (;;) {
        BinaryOp op;
        if (!match_binary_at(level, p_cur(p)->kind, &op)) return left;
        const Token *op_tok = p_advance(p);
        Expr *right = parse_binary(p, level + 1);
        Expr *bin = new_expr(p, EXPR_BINARY, left->loc);
        bin->u.binary.op = op;
        bin->u.binary.left = left;
        bin->u.binary.right = right;
        bin->u.binary.op_loc = op_tok->loc;
        left = bin;
    }
}

static Expr *parse_conditional(Parser *p) {
    Expr *cond = parse_binary(p, 0);
    if (!p_check(p, TOK_QUESTION)) return cond;
    const Token *q = p_advance(p);
    Expr *when_true = parse_conditional(p);
    p_expect(p, TOK_COLON, "':' in the conditional expression");
    Expr *when_false = parse_conditional(p);
    Expr *c = new_expr(p, EXPR_COND, cond->loc);
    c->u.cond.cond = cond;
    c->u.cond.when_true = when_true;
    c->u.cond.when_false = when_false;
    c->u.cond.q_loc = q->loc;
    return c;
}

static Expr *parse_expr(Parser *p) {
    Expr *expr = parse_conditional(p);
    TokenKind k = p_cur(p)->kind;
    if (k == TOK_ASSIGN || k == TOK_PLUS_ASSIGN || k == TOK_MINUS_ASSIGN ||
        k == TOK_STAR_ASSIGN || k == TOK_SLASH_ASSIGN || k == TOK_PERCENT_ASSIGN) {
        p_error(p, "FC2002", p_cur(p)->loc,
            "'%s' cannot be used here: assignment is a statement of its own ('x = e;') and only a plain variable name can be assigned (did you mean '==' to compare?)",
            p_cur(p)->text);
    }
    return expr;
}

/* ---------------------------------------------------------------- statements */

static Expr *parse_paren_cond(Parser *p, const char *kw) {
    p_expect(p, TOK_LPAREN, arena_printf(p->arena, "'(' after '%s'", kw));
    Expr *cond = parse_expr(p);
    p_expect(p, TOK_RPAREN, arena_printf(p->arena, "')' to close the '%s' condition", kw));
    return cond;
}

static Stmt *parse_let(Parser *p) {
    const Token *let_tok = p_advance(p);
    const Token *name = p_expect(p, TOK_IDENT, "a name after 'let'");
    if (p_check(p, TOK_COLON)) {
        p_error(p, "FC2002", p_cur(p)->loc,
            "type annotations on 'let' are not supported; the binding's type is inferred from its initializer");
    }
    p_expect(p, TOK_ASSIGN, arena_printf(p->arena, "'=' after 'let %s'", name->text));
    Expr *init = parse_expr(p);
    Stmt *s = new_stmt(p, STMT_LET, let_tok->loc);
    s->u.let_s.name = name->text;
    s->u.let_s.name_loc = name->loc;
    s->u.let_s.init = init;
    return s;
}

static Stmt *parse_declaration(Parser *p) {
    const Token *type_tok = p_cur(p);
    FcType type = parse_type(p, "a variable declaration");
    if (p_check(p, TOK_STAR)) p_pointer_error(p, p_cur(p));

    VarDeclarator *decls = NULL;
    int count = 0, cap = 0;
    do {
        const Token *name = p_expect(p, TOK_IDENT, "a variable name");
        Expr *init;
        bool has_init;
        if (p_match(p, TOK_ASSIGN)) {
            init = parse_expr(p);
            has_init = true;
        } else {
            if (type == FC_TYPE_BOOL) {
                init = new_expr(p, EXPR_BOOL_LIT, name->loc);
                init->u.bool_val = false;
            } else {
                init = new_expr(p, EXPR_INT_LIT, name->loc);
                init->u.int_val = 0;
            }
            has_init = false;
        }
        if (count >= cap) {
            int nc = cap ? cap * 2 : 4;
            VarDeclarator *nd = arena_alloc(p->arena, (size_t)nc * sizeof(VarDeclarator));
            if (decls) memcpy(nd, decls, (size_t)count * sizeof(VarDeclarator));
            decls = nd;
            cap = nc;
        }
        decls[count].name = name->text;
        decls[count].name_loc = name->loc;
        decls[count].init = init;
        decls[count].has_initializer = has_init;
        decls[count].symbol = NULL;
        count++;
    } while (p_match(p, TOK_COMMA));

    Stmt *s = new_stmt(p, STMT_DECL, type_tok->loc);
    s->u.decl.type = type;
    s->u.decl.decls = decls;
    s->u.decl.count = count;
    return s;
}

static Stmt *parse_simple_stmt(Parser *p) {
    const Token *tok = p_cur(p);
    if (tok->kind == TOK_PLUSPLUS || tok->kind == TOK_MINUSMINUS) {
        p_advance(p);
        const Token *target = p_expect(p, TOK_IDENT, arena_printf(p->arena, "a variable name after '%s'", tok->text));
        Stmt *s = new_stmt(p, STMT_ASSIGN, tok->loc);
        s->u.assign.name = target->text;
        s->u.assign.op = (tok->kind == TOK_PLUSPLUS) ? ASSIGN_INC : ASSIGN_DEC;
        s->u.assign.value = NULL;
        return s;
    }

    if (tok->kind == TOK_IDENT) {
        TokenKind next = p_peek(p, 1)->kind;
        AssignOp op;
        bool is_assign = true;
        switch (next) {
            case TOK_ASSIGN: op = ASSIGN_SET; break;
            case TOK_PLUS_ASSIGN: op = ASSIGN_ADD; break;
            case TOK_MINUS_ASSIGN: op = ASSIGN_SUB; break;
            case TOK_STAR_ASSIGN: op = ASSIGN_MUL; break;
            case TOK_SLASH_ASSIGN: op = ASSIGN_DIV; break;
            case TOK_PERCENT_ASSIGN: op = ASSIGN_REM; break;
            case TOK_PLUSPLUS: op = ASSIGN_INC; break;
            case TOK_MINUSMINUS: op = ASSIGN_DEC; break;
            default: is_assign = false; op = ASSIGN_SET; break;
        }
        if (is_assign) {
            p_advance(p);
            p_advance(p);
            Expr *val = (op == ASSIGN_INC || op == ASSIGN_DEC) ? NULL : parse_expr(p);
            Stmt *s = new_stmt(p, STMT_ASSIGN, tok->loc);
            s->u.assign.name = tok->text;
            s->u.assign.op = op;
            s->u.assign.value = val;
            return s;
        }
    }

    Expr *e = parse_expr(p);
    Stmt *s = new_stmt(p, STMT_EXPR, tok->loc);
    s->u.expr_s.expr = e;
    return s;
}

static void stmt_vec_push(Arena *a, Stmt ***arr, int *count, int *cap, Stmt *item) {
    if (*count >= *cap) {
        int nc = *cap ? *cap * 2 : 8;
        Stmt **na = arena_alloc(a, (size_t)nc * sizeof(Stmt *));
        if (*arr) memcpy(na, *arr, (size_t)(*count) * sizeof(Stmt *));
        *arr = na;
        *cap = nc;
    }
    (*arr)[(*count)++] = item;
}

static Stmt *parse_block(Parser *p, const char *fn_name, const Token *fn_open) {
    const Token *open = p_advance(p); /* '{' */
    Stmt **stmts = NULL;
    int count = 0, cap = 0;
    while (!p_check(p, TOK_RBRACE)) {
        if (p_check(p, TOK_EOF)) {
            p_error(p, "FC2001", p_cur(p)->loc,
                "expected '}' to close the block opened at %d:%d but reached the end of the file",
                open->loc.line, open->loc.column);
        }
        stmt_vec_push(p->arena, &stmts, &count, &cap, parse_stmt(p, fn_name, fn_open));
    }
    p_advance(p);
    Stmt *s = new_stmt(p, STMT_BLOCK, open->loc);
    s->u.block.stmts = stmts;
    s->u.block.count = count;
    return s;
}

static Stmt *parse_for(Parser *p, const char *fn_name, const Token *fn_open) {
    const Token *for_tok = p_advance(p);
    p_expect(p, TOK_LPAREN, "'(' after 'for'");

    Stmt **init = NULL;
    int init_count = 0, init_cap = 0;
    if (is_type_start(p_cur(p)->kind)) {
        stmt_vec_push(p->arena, &init, &init_count, &init_cap, parse_declaration(p));
    } else if (p_check(p, TOK_KW_LET)) {
        stmt_vec_push(p->arena, &init, &init_count, &init_cap, parse_let(p));
    } else if (!p_check(p, TOK_SEMICOLON)) {
        do {
            stmt_vec_push(p->arena, &init, &init_count, &init_cap, parse_simple_stmt(p));
        } while (p_match(p, TOK_COMMA));
    }

    p_expect(p, TOK_SEMICOLON, "';' after the 'for' initializer");
    Expr *cond = p_check(p, TOK_SEMICOLON) ? NULL : parse_expr(p);
    p_expect(p, TOK_SEMICOLON, "';' after the 'for' condition");

    Stmt **update = NULL;
    int update_count = 0, update_cap = 0;
    if (!p_check(p, TOK_RPAREN)) {
        do {
            stmt_vec_push(p->arena, &update, &update_count, &update_cap, parse_simple_stmt(p));
        } while (p_match(p, TOK_COMMA));
    }

    p_expect(p, TOK_RPAREN, "')' to close the 'for' header");
    Stmt *body = parse_stmt(p, fn_name, fn_open);

    Stmt *s = new_stmt(p, STMT_FOR, for_tok->loc);
    s->u.for_s.init = init;
    s->u.for_s.init_count = init_count;
    s->u.for_s.cond = cond;
    s->u.for_s.update = update;
    s->u.for_s.update_count = update_count;
    s->u.for_s.body = body;
    return s;
}

static Stmt *parse_stmt(Parser *p, const char *fn_name, const Token *fn_open) {
    const Token *tok = p_cur(p);
    switch (tok->kind) {
        case TOK_LBRACE:
            return parse_block(p, fn_name, fn_open);
        case TOK_SEMICOLON:
            p_advance(p);
            return new_stmt(p, STMT_EMPTY, tok->loc);
        case TOK_KW_IF: {
            p_advance(p);
            Expr *cond = parse_paren_cond(p, "if");
            Stmt *then_b = parse_stmt(p, fn_name, fn_open);
            Stmt *else_b = NULL;
            if (p_match(p, TOK_KW_ELSE)) {
                else_b = parse_stmt(p, fn_name, fn_open);
            }
            Stmt *s = new_stmt(p, STMT_IF, tok->loc);
            s->u.if_s.cond = cond;
            s->u.if_s.then_branch = then_b;
            s->u.if_s.else_branch = else_b;
            return s;
        }
        case TOK_KW_ELSE:
            p_error(p, "FC2001", tok->loc, "'else' without a matching 'if'");
            return NULL;
        case TOK_KW_WHILE: {
            p_advance(p);
            Expr *cond = parse_paren_cond(p, "while");
            Stmt *body = parse_stmt(p, fn_name, fn_open);
            Stmt *s = new_stmt(p, STMT_WHILE, tok->loc);
            s->u.while_s.cond = cond;
            s->u.while_s.body = body;
            return s;
        }
        case TOK_KW_DO: {
            p_advance(p);
            Stmt *body = parse_stmt(p, fn_name, fn_open);
            p_expect(p, TOK_KW_WHILE, "'while' after the body of 'do'");
            Expr *cond = parse_paren_cond(p, "while");
            p_expect_semi(p);
            Stmt *s = new_stmt(p, STMT_DO_WHILE, tok->loc);
            s->u.do_s.body = body;
            s->u.do_s.cond = cond;
            return s;
        }
        case TOK_KW_FOR:
            return parse_for(p, fn_name, fn_open);
        case TOK_KW_BREAK:
            p_advance(p);
            p_expect_semi(p);
            return new_stmt(p, STMT_BREAK, tok->loc);
        case TOK_KW_CONTINUE:
            p_advance(p);
            p_expect_semi(p);
            return new_stmt(p, STMT_CONTINUE, tok->loc);
        case TOK_KW_RETURN: {
            p_advance(p);
            if (p_check(p, TOK_SEMICOLON)) {
                p_error(p, "FC2001", tok->loc, "'return' requires a value: every function returns 'int' or 'bool'");
            }
            Expr *val = parse_expr(p);
            p_expect_semi(p);
            Stmt *s = new_stmt(p, STMT_RETURN, tok->loc);
            s->u.ret.value = val;
            return s;
        }
        case TOK_KW_LET: {
            Stmt *let_s = parse_let(p);
            p_expect_semi(p);
            return let_s;
        }
        case TOK_KW_INT:
        case TOK_KW_BOOL:
        case TOK_KW_CHAR:
        case TOK_KW_SIGNED:
        case TOK_KW_UNSIGNED: {
            int tlen = type_length(p, 0);
            if (p_peek(p, tlen)->kind == TOK_IDENT && p_peek(p, tlen + 1)->kind == TOK_LPAREN) {
                p_error(p, "FC2001", tok->loc,
                    "missing '}' to close the body of '%s' (opened at %d:%d) before this function definition; nested functions are not supported",
                    fn_name, fn_open->loc.line, fn_open->loc.column);
            }
            Stmt *decl = parse_declaration(p);
            p_expect_semi(p);
            return decl;
        }
        case TOK_RESERVED_WORD:
            p_unsupported(p, tok);
            return NULL;
        default: {
            Stmt *s = parse_simple_stmt(p);
            p_expect_semi(p);
            return s;
        }
    }
}

/* ---------------------------------------------------------------- functions */

static Parameter parse_parameter(Parser *p, const char *ctx) {
    FcType t = parse_type(p, ctx);
    if (p_check(p, TOK_STAR)) p_pointer_error(p, p_cur(p));
    const Token *name = p_expect(p, TOK_IDENT, "a parameter name");
    Parameter param = {t, name->text, name->loc, NULL};
    return param;
}

static FunctionDecl *parse_function(Parser *p) {
    const Token *start = p_cur(p);
    if (start->kind == TOK_KW_LET) {
        p_error(p, "FC2002", start->loc,
            "top-level 'let' bindings are not supported in version 0; only function definitions may appear at top level");
    }

    FcType ret_type = parse_type(p, "a function's return type");
    if (p_check(p, TOK_STAR)) p_pointer_error(p, p_cur(p));

    const Token *name = p_expect(p, TOK_IDENT, "a function name");
    if (p_check(p, TOK_ASSIGN) || p_check(p, TOK_SEMICOLON)) {
        p_error(p, "FC2002", name->loc,
            "global variables are not supported in version 0; only function definitions may appear at top level");
    }

    p_expect(p, TOK_LPAREN, arena_printf(p->arena, "'(' after the function name '%s'", name->text));
    Parameter *params = NULL;
    int param_count = 0, param_cap = 0;
    if (!p_check(p, TOK_RPAREN)) {
        do {
            Parameter pr = parse_parameter(p, "a parameter");
            if (param_count >= param_cap) {
                int nc = param_cap ? param_cap * 2 : 4;
                Parameter *np = arena_alloc(p->arena, (size_t)nc * sizeof(Parameter));
                if (params) memcpy(np, params, (size_t)param_count * sizeof(Parameter));
                params = np;
                param_cap = nc;
            }
            params[param_count++] = pr;
        } while (p_match(p, TOK_COMMA));
    }

    p_expect(p, TOK_RPAREN, "')' to close the parameter list");
    if (p_check(p, TOK_SEMICOLON)) {
        p_error(p, "FC2002", p_cur(p)->loc,
            "function prototypes are not supported in version 0; define '%s' (with a body) before it is used",
            name->text);
    }

    const Token *open = p_expect(p, TOK_LBRACE, arena_printf(p->arena, "'{' to begin the body of '%s'", name->text));
    Stmt **body = NULL;
    int body_count = 0, body_cap = 0;
    while (!p_check(p, TOK_RBRACE)) {
        if (p_check(p, TOK_EOF)) {
            p_error(p, "FC2001", p_cur(p)->loc,
                "expected '}' to close the body of '%s' (opened at %d:%d) but reached the end of the file",
                name->text, open->loc.line, open->loc.column);
        }
        stmt_vec_push(p->arena, &body, &body_count, &body_cap, parse_stmt(p, name->text, open));
    }
    const Token *close = p_advance(p);

    FunctionDecl *fn = arena_alloc(p->arena, sizeof(FunctionDecl));
    fn->return_type = ret_type;
    fn->return_type_loc = start->loc;
    fn->name = name->text;
    fn->name_loc = name->loc;
    fn->params = params;
    fn->param_count = param_count;
    fn->body = body;
    fn->body_count = body_count;
    fn->close_brace_loc = close->loc;
    fn->symbol = NULL;
    return fn;
}

ProgramNode *fc_parse(Arena *a, const TokenList *tokens, const char *file, DiagList *diags) {
    Parser p;
    p.arena = a;
    p.tokens = tokens->items;
    p.count = tokens->count;
    p.index = 0;
    p.file = file;
    p.diags = diags;

    if (setjmp(p.err_jmp) != 0) {
        return NULL;
    }

    FunctionDecl **funcs = NULL;
    int fcount = 0, fcap = 0;
    while (!p_check(&p, TOK_EOF)) {
        FunctionDecl *fn = parse_function(&p);
        if (fcount >= fcap) {
            int nc = fcap ? fcap * 2 : 8;
            FunctionDecl **nf = arena_alloc(a, (size_t)nc * sizeof(FunctionDecl *));
            if (funcs) memcpy(nf, funcs, (size_t)fcount * sizeof(FunctionDecl *));
            funcs = nf;
            fcap = nc;
        }
        funcs[fcount++] = fn;
    }

    ProgramNode *prog = arena_alloc(a, sizeof(ProgramNode));
    prog->file = file;
    prog->functions = funcs;
    prog->func_count = fcount;
    return prog;
}
