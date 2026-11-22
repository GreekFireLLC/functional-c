#include "fc.h"

bool fc_is_always_true(const Expr *cond) {
    if (!cond) return true;
    if (cond->kind == EXPR_BOOL_LIT && cond->u.bool_val) return true;
    if (cond->kind == EXPR_INT_LIT && cond->u.int_val != 0) return true;
    return false;
}

/* ---------------------------------------------------------------- scopes */

typedef struct Scope {
    struct Scope *parent;
    Symbol **symbols;
    int count;
    int cap;
} Scope;

static Scope *scope_new(Arena *a, Scope *parent) {
    Scope *s = arena_alloc(a, sizeof(Scope));
    s->parent = parent;
    s->symbols = NULL;
    s->count = 0;
    s->cap = 0;
    return s;
}

static Symbol *scope_lookup(const Scope *s, const char *name) {
    while (s) {
        for (int i = 0; i < s->count; i++) {
            if (strcmp(s->symbols[i]->name, name) == 0) return s->symbols[i];
        }
        s = s->parent;
    }
    return NULL;
}

static void scope_add(Arena *a, Scope *s, Symbol *sym) {
    if (s->count >= s->cap) {
        int nc = s->cap ? s->cap * 2 : 8;
        Symbol **ns = arena_alloc(a, (size_t)nc * sizeof(Symbol *));
        if (s->symbols) memcpy(ns, s->symbols, (size_t)s->count * sizeof(Symbol *));
        s->symbols = ns;
        s->cap = nc;
    }
    s->symbols[s->count++] = sym;
}

/* ---------------------------------------------------------------- checker state */

typedef struct {
    Symbol *sym;
    SourceLoc at;
} CaptureRef;

typedef struct {
    Arena *arena;
    DiagList *diags;
    bool strict;
    bool dead;
    const char *file;
    ProgramNode *program;

    Symbol **functions;
    int func_count;
    int func_cap;

    Symbol *current;
    const char *pending_name;

    Stmt **loops;
    int loop_count;
    int loop_cap;

    bool in_lambda;

    CaptureRef *captures;
    int cap_count;
    int cap_cap;
} Checker;

static Symbol *new_symbol(Arena *a, SymbolKind kind, const char *name, SourceLoc at) {
    Symbol *s = arena_alloc(a, sizeof(Symbol));
    memset(s, 0, sizeof(Symbol));
    s->kind = kind;
    s->name = name;
    s->c_name = arena_printf(a, "u_%s", name);
    s->declared_at = at;
    return s;
}

static Symbol *find_defined_func(const Checker *c, const char *name) {
    for (int i = 0; i < c->func_count; i++) {
        if (strcmp(c->functions[i]->name, name) == 0) return c->functions[i];
    }
    return NULL;
}

static FunctionDecl *find_any_decl(const Checker *c, const char *name) {
    for (int i = 0; i < c->program->func_count; i++) {
        if (strcmp(c->program->functions[i]->name, name) == 0) return c->program->functions[i];
    }
    return NULL;
}

static void register_func(Checker *c, Symbol *sym) {
    if (c->func_count >= c->func_cap) {
        int nc = c->func_cap ? c->func_cap * 2 : 16;
        Symbol **nf = arena_alloc(c->arena, (size_t)nc * sizeof(Symbol *));
        if (c->functions) memcpy(nf, c->functions, (size_t)c->func_count * sizeof(Symbol *));
        c->functions = nf;
        c->func_cap = nc;
    }
    c->functions[c->func_count++] = sym;
}

static void push_loop(Checker *c, Stmt *loop) {
    if (c->loop_count >= c->loop_cap) {
        int nc = c->loop_cap ? c->loop_cap * 2 : 8;
        Stmt **nl = arena_alloc(c->arena, (size_t)nc * sizeof(Stmt *));
        if (c->loops) memcpy(nl, c->loops, (size_t)c->loop_count * sizeof(Stmt *));
        c->loops = nl;
        c->loop_cap = nc;
    }
    c->loops[c->loop_count++] = loop;
}

static void add_capture(Checker *c, Symbol *sym, SourceLoc at) {
    if (c->cap_count >= c->cap_cap) {
        int nc = c->cap_cap ? c->cap_cap * 2 : 16;
        CaptureRef *nr = arena_alloc(c->arena, (size_t)nc * sizeof(CaptureRef));
        if (c->captures) memcpy(nr, c->captures, (size_t)c->cap_count * sizeof(CaptureRef));
        c->captures = nr;
        c->cap_cap = nc;
    }
    c->captures[c->cap_count].sym = sym;
    c->captures[c->cap_count].at = at;
    c->cap_count++;
}

static void chk_warn(Checker *c, const char *code, SourceLoc at, const char *strict_msg, const char *loose_msg) {
    diag_add(c->diags, c->arena, code, at, !c->strict, "%s", c->strict ? strict_msg : loose_msg);
}

static bool check_new_name(Checker *c, const char *name, SourceLoc at, Scope *scope, const char *kind) {
    if (strcmp(name, "print") == 0) {
        diag_add(c->diags, c->arena, "FC3012", at, false,
            "'print' is a built-in function and cannot be used as a %s name", kind);
        return false;
    }
    if (c->pending_name && strcmp(name, c->pending_name) == 0) {
        diag_add(c->diags, c->arena, "FC3003", at, false,
            "%s '%s' has the same name as the binding being defined; shadowing is not allowed", kind, name);
        return false;
    }
    Symbol *existing = scope_lookup(scope, name);
    if (existing) {
        diag_add(c->diags, c->arena, "FC3003", at, false,
            "'%s' is already defined (at %d:%d) in this block or an enclosing one; redeclaring or shadowing a name is not allowed, so choose another name",
            name, existing->declared_at.line, existing->declared_at.column);
        return false;
    }
    Symbol *fn = find_defined_func(c, name);
    if (!fn && c->current && strcmp(c->current->name, name) == 0) fn = c->current;
    if (fn) {
        diag_add(c->diags, c->arena, "FC3003", at, false,
            "%s '%s' would shadow the function '%s' defined at %d:%d; shadowing is not allowed",
            kind, name, name, fn->declared_at.line, fn->declared_at.column);
        return false;
    }
    return true;
}

static Symbol *resolve_name(Checker *c, const char *name, SourceLoc at, Scope *scope) {
    Symbol *local = scope_lookup(scope, name);
    if (local) return local;
    if (c->pending_name && strcmp(name, c->pending_name) == 0) {
        diag_add(c->diags, c->arena, "FC3015", at, false,
            "'%s' cannot be used inside its own initializer; recursive bindings are not supported", name);
        return NULL;
    }
    Symbol *fn = find_defined_func(c, name);
    if (fn) return fn;
    if (c->current && strcmp(c->current->name, name) == 0) return c->current;
    FunctionDecl *later = find_any_decl(c, name);
    if (later) {
        diag_add(c->diags, c->arena, "FC3004", at, false,
            "'%s' is defined later (at %d:%d); forward references and mutual recursion are not supported yet, so define '%s' before this use",
            name, later->name_loc.line, later->name_loc.column, name);
        return NULL;
    }
    diag_add(c->diags, c->arena, "FC3001", at, false, "unknown name '%s'", name);
    return NULL;
}

static bool accepts(Checker *c, FcType expected, Expr *expr, FcType actual) {
    if (actual == FC_TYPE_NONE || actual == expected) return true;
    if (c->strict && expected == FC_TYPE_BOOL) return false;
    expr->convert_to = expected;
    return true;
}

static void promote(Expr *operand, FcType actual) {
    if (actual != FC_TYPE_NONE && actual != FC_TYPE_INT) {
        operand->convert_to = FC_TYPE_INT;
    }
}

/* Forward declarations */
static FcType check_expr(Checker *c, Expr *expr, Scope *scope);
static bool check_stmt(Checker *c, Stmt *stmt, Scope *scope);

/* ---------------------------------------------------------------- expressions */

static FcType check_lambda(Checker *c, Expr *lambda, Scope *scope) {
    Scope *lscope = scope_new(c->arena, scope);
    for (int i = 0; i < lambda->u.lambda.param_count; i++) {
        Parameter *p = &lambda->u.lambda.params[i];
        if (check_new_name(c, p->name, p->loc, lscope, "lambda parameter")) {
            Symbol *sym = new_symbol(c->arena, SYM_VALUE, p->name, p->loc);
            sym->value_type = p->type;
            sym->value_kind = "lambda parameter";
            sym->is_mutable = false;
            p->symbol = sym;
            scope_add(c->arena, lscope, sym);
        }
    }
    bool prev_in_lambda = c->in_lambda;
    c->in_lambda = true;
    FcType ret = check_expr(c, lambda->u.lambda.body, lscope);
    c->in_lambda = prev_in_lambda;
    return ret;
}

static FcType check_call(Checker *c, Expr *call, Scope *scope) {
    Symbol *sym = resolve_name(c, call->u.call.callee, call->loc, scope);
    int argc = call->u.call.arg_count;
    FcType *arg_types = argc > 0 ? arena_alloc(c->arena, (size_t)argc * sizeof(FcType)) : NULL;
    for (int i = 0; i < argc; i++) {
        arg_types[i] = check_expr(c, call->u.call.args[i], scope);
    }
    call->u.call.symbol = sym;

    if (!sym) return FC_TYPE_NONE;
    if (sym->kind == SYM_VALUE) {
        if (sym->value_type == FC_TYPE_NONE) return FC_TYPE_NONE;
        diag_add(c->diags, c->arena, "FC3010", call->loc, false,
            "'%s' is a %s of type %s, not a function, so it cannot be called",
            call->u.call.callee, sym->value_kind, fc_type_name(sym->value_type));
        return FC_TYPE_NONE;
    }

    if (sym == c->current && !c->dead) {
        c->current->is_recursive = true;
    }

    if (argc != sym->param_count) {
        diag_add(c->diags, c->arena, "FC3008", call->loc, false,
            "'%s' expects %d argument(s) but was given %d",
            call->u.call.callee, sym->param_count, argc);
        return sym->return_type;
    }

    for (int i = 0; i < argc; i++) {
        if (arg_types[i] != FC_TYPE_NONE && !accepts(c, sym->param_types[i], call->u.call.args[i], arg_types[i])) {
            diag_add(c->diags, c->arena, "FC3007", call->u.call.args[i]->loc, false,
                "argument %d of '%s' must be %s but has type %s",
                i + 1, call->u.call.callee, fc_type_name(sym->param_types[i]), fc_type_name(arg_types[i]));
        }
    }
    return sym->return_type;
}

static FcType check_binary(Checker *c, Expr *bin, Scope *scope) {
    FcType left = check_expr(c, bin->u.binary.left, scope);
    FcType right = check_expr(c, bin->u.binary.right, scope);
    switch (bin->u.binary.op) {
        case BINOP_ADD:
        case BINOP_SUB:
        case BINOP_MUL:
        case BINOP_DIV:
        case BINOP_REM:
            promote(bin->u.binary.left, left);
            promote(bin->u.binary.right, right);
            return FC_TYPE_INT;
        case BINOP_LT:
        case BINOP_LE:
        case BINOP_GT:
        case BINOP_GE:
            promote(bin->u.binary.left, left);
            promote(bin->u.binary.right, right);
            return FC_TYPE_BOOL;
        case BINOP_EQ:
        case BINOP_NE:
            if (left != FC_TYPE_NONE && right != FC_TYPE_NONE && left != right) {
                promote(bin->u.binary.left, left);
                promote(bin->u.binary.right, right);
            }
            return FC_TYPE_BOOL;
        case BINOP_AND:
        case BINOP_OR:
            return FC_TYPE_BOOL;
    }
    return FC_TYPE_NONE;
}

static FcType check_expr_core(Checker *c, Expr *expr, Scope *scope) {
    switch (expr->kind) {
        case EXPR_INT_LIT:
            return FC_TYPE_INT;
        case EXPR_BOOL_LIT:
            return FC_TYPE_BOOL;
        case EXPR_NAME: {
            if (strcmp(expr->u.name.name, "print") == 0) {
                diag_add(c->diags, c->arena, "FC3013", expr->loc, false,
                    "'print' is a built-in statement, not a value; call it as 'print(value);'");
                return FC_TYPE_NONE;
            }
            Symbol *sym = resolve_name(c, expr->u.name.name, expr->loc, scope);
            expr->u.name.symbol = sym;
            if (!sym) return FC_TYPE_NONE;
            if (sym->kind == SYM_VALUE) {
                if (c->in_lambda && !c->dead && strcmp(sym->value_kind, "lambda parameter") != 0) {
                    add_capture(c, sym, expr->loc);
                }
                return sym->value_type;
            }
            diag_add(c->diags, c->arena, "FC3009", expr->loc, false,
                "'%s' is a function; it can only be called, as in '%s(...)' (functions are not values yet)",
                expr->u.name.name, expr->u.name.name);
            return FC_TYPE_NONE;
        }
        case EXPR_UNARY:
            if (expr->u.unary.op == UNOP_NOT) {
                check_expr(c, expr->u.unary.operand, scope);
                return FC_TYPE_BOOL;
            }
            promote(expr->u.unary.operand, check_expr(c, expr->u.unary.operand, scope));
            return FC_TYPE_INT;
        case EXPR_CAST: {
            FcType from = check_expr(c, expr->u.cast.operand, scope);
            if (from != FC_TYPE_NONE && from != expr->u.cast.target) {
                expr->u.cast.operand->convert_to = expr->u.cast.target;
            }
            return expr->u.cast.target;
        }
        case EXPR_BINARY:
            return check_binary(c, expr, scope);
        case EXPR_COND: {
            check_expr(c, expr->u.cond.cond, scope);
            FcType wt = check_expr(c, expr->u.cond.when_true, scope);
            FcType wf = check_expr(c, expr->u.cond.when_false, scope);
            if (wt != FC_TYPE_NONE && wf != FC_TYPE_NONE && wt != wf) {
                promote(expr->u.cond.when_true, wt);
                promote(expr->u.cond.when_false, wf);
                return FC_TYPE_INT;
            }
            return wt != FC_TYPE_NONE ? wt : wf;
        }
        case EXPR_CALL:
            if (strcmp(expr->u.call.callee, "print") == 0) {
                for (int i = 0; i < expr->u.call.arg_count; i++) {
                    check_expr(c, expr->u.call.args[i], scope);
                }
                diag_add(c->diags, c->arena, "FC3013", expr->loc, false,
                    "'print(...)' produces no value; it can only be used as a statement, as in 'print(x);'");
                return FC_TYPE_NONE;
            }
            return check_call(c, expr, scope);
        case EXPR_LAMBDA:
            diag_add(c->diags, c->arena, "FC3014", expr->loc, false,
                "a lambda can only be the entire initializer of a 'let' binding, as in 'let f = (int x) => x + 1;'");
            return FC_TYPE_NONE;
    }
    return FC_TYPE_NONE;
}

static FcType check_expr(Checker *c, Expr *expr, Scope *scope) {
    FcType t = check_expr_core(c, expr, scope);
    expr->type = t;
    return t;
}

/* ---------------------------------------------------------------- statements */

static bool check_stmts(Checker *c, Stmt **stmts, int count, Scope *scope, Stmt ***out_live, int *out_live_count) {
    bool reachable = true;
    bool reported = false;
    bool enclosing_dead = c->dead;
    int live_count = count;

    for (int i = 0; i < count; i++) {
        Stmt *st = stmts[i];
        if (!reachable && !reported && !enclosing_dead && st->kind != STMT_EMPTY) {
            const char *why = "unreachable statement: the statement before it never finishes normally (it returns, breaks, continues, or loops forever)";
            const char *loose = arena_printf(c->arena, "%s; it is left out of the translation", why);
            chk_warn(c, "FC3016", st->loc, why, loose);
            reported = true;
        }
        if (!check_stmt(c, st, scope) && reachable) {
            reachable = false;
            live_count = i + 1;
            c->dead = true;
        }
    }

    c->dead = enclosing_dead;
    *out_live = stmts;
    *out_live_count = live_count;
    return reachable;
}

static bool check_loop_body(Checker *c, Stmt *loop, Stmt *body, Scope *scope) {
    push_loop(c, loop);
    bool completes = check_stmt(c, body, scope_new(c->arena, scope));
    c->loop_count--;
    return completes;
}

static bool check_stmt(Checker *c, Stmt *st, Scope *scope) {
    bool completes = true;
    switch (st->kind) {
        case STMT_EMPTY:
            completes = true;
            break;
        case STMT_BLOCK: {
            Stmt **live;
            int live_count;
            completes = check_stmts(c, st->u.block.stmts, st->u.block.count, scope_new(c->arena, scope), &live, &live_count);
            st->u.block.stmts = live;
            st->u.block.count = live_count;
            break;
        }
        case STMT_LET: {
            bool name_ok = check_new_name(c, st->u.let_s.name, st->u.let_s.name_loc, scope, "let binding");
            c->pending_name = st->u.let_s.name;
            Symbol *sym;
            if (st->u.let_s.init->kind == EXPR_LAMBDA) {
                Expr *lam = st->u.let_s.init;
                FcType ret = check_lambda(c, lam, scope);
                sym = new_symbol(c->arena, SYM_LAMBDA, st->u.let_s.name, st->u.let_s.name_loc);
                sym->param_count = lam->u.lambda.param_count;
                if (sym->param_count > 0) {
                    sym->param_types = arena_alloc(c->arena, (size_t)sym->param_count * sizeof(FcType));
                    for (int i = 0; i < sym->param_count; i++) {
                        sym->param_types[i] = lam->u.lambda.params[i].type;
                    }
                }
                sym->return_type = ret;
            } else {
                FcType t = check_expr(c, st->u.let_s.init, scope);
                sym = new_symbol(c->arena, SYM_VALUE, st->u.let_s.name, st->u.let_s.name_loc);
                sym->value_type = t;
                sym->value_kind = "let binding";
                sym->is_mutable = false;
            }
            c->pending_name = NULL;
            st->u.let_s.symbol = sym;
            if (name_ok) scope_add(c->arena, scope, sym);
            completes = true;
            break;
        }
        case STMT_DECL: {
            FcType decl_type = st->u.decl.type;
            for (int i = 0; i < st->u.decl.count; i++) {
                VarDeclarator *d = &st->u.decl.decls[i];
                bool name_ok = check_new_name(c, d->name, d->name_loc, scope, "variable");
                if (c->strict && !d->has_initializer) {
                    diag_add(c->diags, c->arena, "FC2002", d->name_loc, false,
                        "variable '%s' must be initialized, as in '%s %s = ...;' (--strict rejects uninitialized variables)",
                        d->name, fc_type_name(decl_type), d->name);
                }
                c->pending_name = d->name;
                FcType t = check_expr(c, d->init, scope);
                c->pending_name = NULL;
                if (!accepts(c, decl_type, d->init, t)) {
                    diag_add(c->diags, c->arena, "FC3007", d->init->loc, false,
                        "cannot initialize the %s variable '%s' with a value of type %s",
                        fc_type_name(decl_type), d->name, fc_type_name(t));
                }
                Symbol *sym = new_symbol(c->arena, SYM_VALUE, d->name, d->name_loc);
                sym->value_type = decl_type;
                sym->value_kind = "variable";
                sym->is_mutable = true;
                d->symbol = sym;
                if (name_ok) scope_add(c->arena, scope, sym);
            }
            completes = true;
            break;
        }
        case STMT_ASSIGN: {
            FcType vtype = st->u.assign.value ? check_expr(c, st->u.assign.value, scope) : FC_TYPE_NONE;
            Symbol *sym = scope_lookup(scope, st->u.assign.name);
            if (!sym) sym = find_defined_func(c, st->u.assign.name);
            const char *op = assignop_source(st->u.assign.op);

            if (sym && sym->kind == SYM_VALUE && sym->is_mutable) {
                sym->is_assigned = true;
                st->u.assign.symbol = sym;
                if (st->u.assign.op == ASSIGN_SET) {
                    if (sym->value_type != FC_TYPE_NONE && !accepts(c, sym->value_type, st->u.assign.value, vtype)) {
                        diag_add(c->diags, c->arena, "FC3007", st->u.assign.value->loc, false,
                            "cannot assign a value of type %s to the %s %s '%s'",
                            fc_type_name(vtype), fc_type_name(sym->value_type), sym->value_kind, st->u.assign.name);
                    }
                } else {
                    if (sym->value_type == FC_TYPE_BOOL) {
                        diag_add(c->diags, c->arena, "FC3007", st->loc, false,
                            "'%s' needs an int or char variable, but '%s' is bool", op, st->u.assign.name);
                    }
                    if (st->u.assign.value) {
                        promote(st->u.assign.value, vtype);
                    }
                }
            } else if (sym && sym->kind == SYM_VALUE) {
                if (strcmp(sym->value_kind, "lambda parameter") == 0) {
                    diag_add(c->diags, c->arena, "FC3002", st->loc, false,
                        "cannot assign to '%s': lambda parameters are immutable", st->u.assign.name);
                } else {
                    diag_add(c->diags, c->arena, "FC3002", st->loc, false,
                        "cannot assign to '%s': 'let' bindings are immutable ('%s' was bound at %d:%d); declare it as '%s %s = ...;' if it needs to change",
                        st->u.assign.name, st->u.assign.name,
                        sym->declared_at.line, sym->declared_at.column,
                        fc_type_name(sym->value_type), st->u.assign.name);
                }
            } else if (sym && sym->kind == SYM_LAMBDA) {
                diag_add(c->diags, c->arena, "FC3002", st->loc, false,
                    "cannot assign to '%s': it is a 'let'-bound lambda (bound at %d:%d) and bindings are immutable",
                    st->u.assign.name, sym->declared_at.line, sym->declared_at.column);
            } else if (sym && sym->kind == SYM_FUNCTION) {
                diag_add(c->diags, c->arena, "FC3002", st->loc, false,
                    "cannot assign to the function '%s' (defined at %d:%d)",
                    st->u.assign.name, sym->declared_at.line, sym->declared_at.column);
            } else {
                diag_add(c->diags, c->arena, "FC3001", st->loc, false,
                    "cannot assign to unknown name '%s'; declare it first, e.g. 'int %s = 0;'",
                    st->u.assign.name, st->u.assign.name);
            }
            completes = true;
            break;
        }
        case STMT_EXPR: {
            Expr *e = st->u.expr_s.expr;
            if (e->kind == EXPR_CALL && strcmp(e->u.call.callee, "print") == 0) {
                e->u.call.is_print = true;
                int argc = e->u.call.arg_count;
                FcType first_type = FC_TYPE_NONE;
                for (int i = 0; i < argc; i++) {
                    FcType t = check_expr(c, e->u.call.args[i], scope);
                    if (i == 0) first_type = t;
                }
                if (argc != 1) {
                    diag_add(c->diags, c->arena, "FC3008", e->loc, false,
                        "'print' expects exactly 1 argument (an int) but was given %d", argc);
                } else {
                    promote(e->u.call.args[0], first_type);
                }
            } else if (e->kind == EXPR_CALL) {
                check_call(c, e, scope);
            } else {
                check_expr(c, e, scope);
                diag_add(c->diags, c->arena, "FC3011", st->loc, false,
                    "only calls, assignments and increments can be used as statements; this expression's value would be discarded");
            }
            completes = true;
            break;
        }
        case STMT_RETURN: {
            FcType t = check_expr(c, st->u.ret.value, scope);
            FunctionDecl *fn = c->current->func_decl;
            if (!accepts(c, fn->return_type, st->u.ret.value, t)) {
                diag_add(c->diags, c->arena, "FC3007", st->u.ret.value->loc, false,
                    "function '%s' returns %s but this expression has type %s",
                    fn->name, fc_type_name(fn->return_type), fc_type_name(t));
            }
            completes = false;
            break;
        }
        case STMT_IF: {
            check_expr(c, st->u.if_s.cond, scope);
            bool tc = check_stmt(c, st->u.if_s.then_branch, scope_new(c->arena, scope));
            bool ec = st->u.if_s.else_branch ? check_stmt(c, st->u.if_s.else_branch, scope_new(c->arena, scope)) : true;
            completes = tc || ec;
            break;
        }
        case STMT_WHILE: {
            check_expr(c, st->u.while_s.cond, scope);
            check_loop_body(c, st, st->u.while_s.body, scope);
            completes = !fc_is_always_true(st->u.while_s.cond) || st->uses_break;
            break;
        }
        case STMT_DO_WHILE: {
            bool bc = check_loop_body(c, st, st->u.do_s.body, scope);
            check_expr(c, st->u.do_s.cond, scope);
            bool cond_reachable = bc || st->uses_continue;
            completes = st->uses_break || (cond_reachable && !fc_is_always_true(st->u.do_s.cond));
            break;
        }
        case STMT_FOR: {
            Scope *lscope = scope_new(c->arena, scope);
            for (int i = 0; i < st->u.for_s.init_count; i++) {
                check_stmt(c, st->u.for_s.init[i], lscope);
            }
            if (st->u.for_s.cond) {
                check_expr(c, st->u.for_s.cond, lscope);
            }
            check_loop_body(c, st, st->u.for_s.body, lscope);
            for (int i = 0; i < st->u.for_s.update_count; i++) {
                check_stmt(c, st->u.for_s.update[i], lscope);
            }
            completes = !fc_is_always_true(st->u.for_s.cond) || st->uses_break;
            break;
        }
        case STMT_BREAK:
        case STMT_CONTINUE: {
            const char *kw = (st->kind == STMT_BREAK) ? "break" : "continue";
            if (c->loop_count == 0) {
                diag_add(c->diags, c->arena, "FC3018", st->loc, false,
                    "'%s' can only be used inside a loop (while, do ... while, or for)", kw);
                completes = true;
            } else {
                if (!c->dead) {
                    if (st->kind == STMT_BREAK) c->loops[c->loop_count - 1]->uses_break = true;
                    else c->loops[c->loop_count - 1]->uses_continue = true;
                }
                completes = false;
            }
            break;
        }
    }
    st->can_complete_normally = completes;
    return completes;
}

/* ---------------------------------------------------------------- functions & entry point */

static void check_function(Checker *c, FunctionDecl *fn) {
    Symbol *sym = new_symbol(c->arena, SYM_FUNCTION, fn->name, fn->name_loc);
    sym->func_decl = fn;
    sym->param_count = fn->param_count;
    if (fn->param_count > 0) {
        sym->param_types = arena_alloc(c->arena, (size_t)fn->param_count * sizeof(FcType));
        for (int i = 0; i < fn->param_count; i++) {
            sym->param_types[i] = fn->params[i].type;
        }
    }
    sym->return_type = fn->return_type;
    fn->symbol = sym;

    if (strcmp(fn->name, "print") == 0) {
        diag_add(c->diags, c->arena, "FC3012", fn->name_loc, false,
            "'print' is a built-in function and cannot be redefined");
    } else {
        Symbol *prev = find_defined_func(c, fn->name);
        if (prev) {
            if (strcmp(fn->name, "main") != 0) {
                diag_add(c->diags, c->arena, "FC3003", fn->name_loc, false,
                    "function '%s' is already defined at %d:%d",
                    fn->name, prev->declared_at.line, prev->declared_at.column);
            }
        } else {
            register_func(c, sym);
        }
    }

    c->current = sym;
    c->cap_count = 0;
    Scope *scope = scope_new(c->arena, NULL);
    for (int i = 0; i < fn->param_count; i++) {
        Parameter *p = &fn->params[i];
        if (check_new_name(c, p->name, p->loc, scope, "parameter")) {
            Symbol *psym = new_symbol(c->arena, SYM_VALUE, p->name, p->loc);
            psym->value_type = p->type;
            psym->value_kind = "parameter";
            psym->is_mutable = true;
            p->symbol = psym;
            scope_add(c->arena, scope, psym);
        }
    }

    Stmt **live_body;
    int live_count;
    bool falls_off = check_stmts(c, fn->body, fn->body_count, scope, &live_body, &live_count);
    if (falls_off) {
        bool is_bool = (fn->return_type == FC_TYPE_BOOL);
        if (c->strict || strcmp(fn->name, "main") != 0) {
            const char *smsg = arena_printf(c->arena,
                "function '%s' can reach its end without returning a value; every path must end with 'return <expression>;'",
                fn->name);
            const char *lmsg = arena_printf(c->arena,
                "function '%s' can reach its end without returning a value; it returns %s there (C leaves the value undefined)",
                fn->name, is_bool ? "false" : "0");
            chk_warn(c, "FC3005", fn->close_brace_loc, smsg, lmsg);
        }

        Expr *val = arena_alloc(c->arena, sizeof(Expr));
        memset(val, 0, sizeof(Expr));
        val->loc = fn->close_brace_loc;
        if (is_bool) {
            val->kind = EXPR_BOOL_LIT;
            val->type = FC_TYPE_BOOL;
            val->u.bool_val = false;
        } else {
            val->kind = EXPR_INT_LIT;
            val->type = FC_TYPE_INT;
            val->u.int_val = 0;
        }
        accepts(c, fn->return_type, val, val->type);

        Stmt *ret = arena_alloc(c->arena, sizeof(Stmt));
        memset(ret, 0, sizeof(Stmt));
        ret->kind = STMT_RETURN;
        ret->loc = fn->close_brace_loc;
        ret->can_complete_normally = false;
        ret->u.ret.value = val;

        Stmt **nb = arena_alloc(c->arena, (size_t)(live_count + 1) * sizeof(Stmt *));
        if (live_count > 0) memcpy(nb, live_body, (size_t)live_count * sizeof(Stmt *));
        nb[live_count] = ret;
        live_body = nb;
        live_count++;
    }
    fn->body = live_body;
    fn->body_count = live_count;

    for (int i = 0; i < c->cap_count; i++) {
        Symbol *cap = c->captures[i].sym;
        if (cap->is_assigned) {
            diag_add(c->diags, c->arena, "FC3017", c->captures[i].at, false,
                "a lambda cannot capture '%s' (declared at %d:%d) because it is a mutable variable that is assigned; copy its current value into an immutable binding first, e.g. 'let %sValue = %s;'",
                cap->name, cap->declared_at.line, cap->declared_at.column, cap->name, cap->name);
        }
    }
    c->cap_count = 0;
    c->current = NULL;
}

static void check_entry_point(Checker *c) {
    FunctionDecl *first_main = NULL;
    for (int i = 0; i < c->program->func_count; i++) {
        FunctionDecl *fn = c->program->functions[i];
        if (strcmp(fn->name, "main") == 0) {
            if (!first_main) {
                first_main = fn;
            } else {
                diag_add(c->diags, c->arena, "FC3006", fn->name_loc, false,
                    "duplicate 'main': the program must define exactly one 'int main()' (first defined at %d:%d)",
                    first_main->name_loc.line, first_main->name_loc.column);
            }
        }
    }
    if (!first_main) {
        SourceLoc start = {c->file, 1, 1};
        diag_add(c->diags, c->arena, "FC3006", start, false,
            "the program must define exactly one 'int main()'; no 'main' function was found");
        return;
    }
    if (first_main->return_type != FC_TYPE_INT) {
        diag_add(c->diags, c->arena, "FC3006", first_main->return_type_loc, false,
            "'main' must return 'int' (its value is the process exit code), not '%s'",
            fc_type_name(first_main->return_type));
    }
    if (first_main->param_count > 0) {
        diag_add(c->diags, c->arena, "FC3006", first_main->params[0].loc, false,
            "'main' must not take parameters");
    }
}

void fc_check(Arena *a, ProgramNode *program, bool strict, DiagList *diags) {
    Checker c;
    memset(&c, 0, sizeof(c));
    c.arena = a;
    c.diags = diags;
    c.strict = strict;
    c.file = program->file;
    c.program = program;

    for (int i = 0; i < program->func_count; i++) {
        check_function(&c, program->functions[i]);
    }
    check_entry_point(&c);
    diag_sort(diags);
}
