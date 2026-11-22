#include "fc.h"

/* ---------------------------------------------------------------- arena line list */

typedef struct {
    const char **items;
    int count;
    int cap;
} LineList;

static void ll_init(LineList *ll) {
    ll->items = NULL;
    ll->count = 0;
    ll->cap = 0;
}

static void ll_push(Arena *a, LineList *ll, const char *line) {
    if (ll->count >= ll->cap) {
        int nc = ll->cap ? ll->cap * 2 : 16;
        const char **ni = arena_alloc(a, (size_t)nc * sizeof(const char *));
        if (ll->items) memcpy(ni, ll->items, (size_t)ll->count * sizeof(const char *));
        ll->items = ni;
        ll->cap = nc;
    }
    ll->items[ll->count++] = line;
}

static void ll_push_indented(Arena *a, LineList *dst, const LineList *src) {
    for (int i = 0; i < src->count; i++) {
        ll_push(a, dst, arena_printf(a, "    %s", src->items[i]));
    }
}

static void ll_push_all(Arena *a, LineList *dst, const LineList *src) {
    for (int i = 0; i < src->count; i++) {
        ll_push(a, dst, src->items[i]);
    }
}

/* ---------------------------------------------------------------- symbol pointer set */

typedef struct {
    const Symbol **items;
    int count;
    int cap;
} SymSet;

static void symset_init(SymSet *s) {
    s->items = NULL;
    s->count = 0;
    s->cap = 0;
}

static bool symset_contains(const SymSet *s, const Symbol *sym) {
    for (int i = 0; i < s->count; i++) {
        if (s->items[i] == sym) return true;
    }
    return false;
}

static bool symset_add(Arena *a, SymSet *s, const Symbol *sym) {
    if (symset_contains(s, sym)) return false;
    if (s->count >= s->cap) {
        int nc = s->cap ? s->cap * 2 : 16;
        const Symbol **ni = arena_alloc(a, (size_t)nc * sizeof(const Symbol *));
        if (s->items) memcpy(ni, s->items, (size_t)s->count * sizeof(const Symbol *));
        s->items = ni;
        s->cap = nc;
    }
    s->items[s->count++] = sym;
    return true;
}

/* ---------------------------------------------------------------- lifted lambdas */

typedef struct {
    const char *c_func_name;
    const Symbol *symbol;
    const Expr *expr;
    SymSet captures;
} LiftedLambda;

typedef struct {
    Arena *arena;
    StrBuf *out;
    int temp_counter;
    int loop_flag_counter;

    LiftedLambda *lambdas;
    int lambda_count;
    int lambda_cap;

    SymSet read_values;
} Emitter;

static const LiftedLambda *find_lifted(const Emitter *e, const Symbol *sym) {
    for (int i = 0; i < e->lambda_count; i++) {
        if (e->lambdas[i].symbol == sym) return &e->lambdas[i];
    }
    return NULL;
}

static const char *c_type_name(FcType t) {
    switch (t) {
        case FC_TYPE_INT: return "int32_t";
        case FC_TYPE_BOOL: return "bool";
        case FC_TYPE_CHAR: return "int8_t";
        case FC_TYPE_UCHAR: return "uint8_t";
        default: return "int32_t";
    }
}

static const char *func_header(Arena *a, const FunctionDecl *fn) {
    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "%s %s(", c_type_name(fn->return_type), fn->symbol->c_name);
    if (fn->param_count == 0) {
        sb_append(&sb, "void");
    } else {
        for (int i = 0; i < fn->param_count; i++) {
            if (i > 0) sb_append(&sb, ", ");
            sb_printf(&sb, "%s %s", c_type_name(fn->params[i].type), fn->params[i].symbol->c_name);
        }
    }
    sb_append_char(&sb, ')');
    char *res = arena_strndup(a, sb.data, sb.len);
    sb_free(&sb);
    return res;
}

/* ---------------------------------------------------------------- lambda lifting & read analysis */

static void scan_lambda_body(
    Emitter *e,
    const Expr *expr,
    const Symbol *enclosing,
    const SymSet *own_params,
    SymSet *captures,
    bool *calls_enclosing)
{
    switch (expr->kind) {
        case EXPR_NAME:
            if (expr->u.name.symbol && expr->u.name.symbol->kind == SYM_VALUE) {
                if (!symset_contains(own_params, expr->u.name.symbol)) {
                    symset_add(e->arena, captures, expr->u.name.symbol);
                }
            }
            break;
        case EXPR_UNARY:
            scan_lambda_body(e, expr->u.unary.operand, enclosing, own_params, captures, calls_enclosing);
            break;
        case EXPR_CAST:
            scan_lambda_body(e, expr->u.cast.operand, enclosing, own_params, captures, calls_enclosing);
            break;
        case EXPR_BINARY:
            scan_lambda_body(e, expr->u.binary.left, enclosing, own_params, captures, calls_enclosing);
            scan_lambda_body(e, expr->u.binary.right, enclosing, own_params, captures, calls_enclosing);
            break;
        case EXPR_COND:
            scan_lambda_body(e, expr->u.cond.cond, enclosing, own_params, captures, calls_enclosing);
            scan_lambda_body(e, expr->u.cond.when_true, enclosing, own_params, captures, calls_enclosing);
            scan_lambda_body(e, expr->u.cond.when_false, enclosing, own_params, captures, calls_enclosing);
            break;
        case EXPR_CALL:
            if (expr->u.call.symbol == enclosing) {
                *calls_enclosing = true;
            } else if (expr->u.call.symbol && expr->u.call.symbol->kind == SYM_LAMBDA) {
                const LiftedLambda *callee = find_lifted(e, expr->u.call.symbol);
                if (callee) {
                    for (int i = 0; i < callee->captures.count; i++) {
                        if (!symset_contains(own_params, callee->captures.items[i])) {
                            symset_add(e->arena, captures, callee->captures.items[i]);
                        }
                    }
                }
            }
            for (int i = 0; i < expr->u.call.arg_count; i++) {
                scan_lambda_body(e, expr->u.call.args[i], enclosing, own_params, captures, calls_enclosing);
            }
            break;
        default:
            break;
    }
}

static void collect_lambdas_stmt(Emitter *e, const FunctionDecl *enclosing, const Stmt *st, bool *calls_enclosing);

static void collect_lambdas_list(Emitter *e, const FunctionDecl *enclosing, Stmt *const *stmts, int count, bool *calls_enclosing) {
    for (int i = 0; i < count; i++) {
        collect_lambdas_stmt(e, enclosing, stmts[i], calls_enclosing);
    }
}

static void collect_lambdas_stmt(Emitter *e, const FunctionDecl *enclosing, const Stmt *st, bool *calls_enclosing) {
    if (!st) return;
    switch (st->kind) {
        case STMT_BLOCK:
            collect_lambdas_list(e, enclosing, st->u.block.stmts, st->u.block.count, calls_enclosing);
            break;
        case STMT_IF:
            collect_lambdas_stmt(e, enclosing, st->u.if_s.then_branch, calls_enclosing);
            if (st->u.if_s.else_branch) collect_lambdas_stmt(e, enclosing, st->u.if_s.else_branch, calls_enclosing);
            break;
        case STMT_WHILE:
            collect_lambdas_stmt(e, enclosing, st->u.while_s.body, calls_enclosing);
            break;
        case STMT_DO_WHILE:
            collect_lambdas_stmt(e, enclosing, st->u.do_s.body, calls_enclosing);
            break;
        case STMT_FOR:
            collect_lambdas_list(e, enclosing, st->u.for_s.init, st->u.for_s.init_count, calls_enclosing);
            collect_lambdas_stmt(e, enclosing, st->u.for_s.body, calls_enclosing);
            break;
        case STMT_LET:
            if (st->u.let_s.init->kind == EXPR_LAMBDA) {
                const Expr *lam = st->u.let_s.init;
                SymSet own_params;
                symset_init(&own_params);
                for (int i = 0; i < lam->u.lambda.param_count; i++) {
                    symset_add(e->arena, &own_params, lam->u.lambda.params[i].symbol);
                }
                SymSet captures;
                symset_init(&captures);
                scan_lambda_body(e, lam->u.lambda.body, enclosing->symbol, &own_params, &captures, calls_enclosing);

                if (e->lambda_count >= e->lambda_cap) {
                    int nc = e->lambda_cap ? e->lambda_cap * 2 : 8;
                    LiftedLambda *nl = arena_alloc(e->arena, (size_t)nc * sizeof(LiftedLambda));
                    if (e->lambdas) memcpy(nl, e->lambdas, (size_t)e->lambda_count * sizeof(LiftedLambda));
                    e->lambdas = nl;
                    e->lambda_cap = nc;
                }
                LiftedLambda *ll = &e->lambdas[e->lambda_count++];
                ll->c_func_name = arena_printf(e->arena, "fc_lam_%s_%d_%s", enclosing->name, e->lambda_count, st->u.let_s.name);
                ll->symbol = st->u.let_s.symbol;
                ll->expr = lam;
                ll->captures = captures;
            }
            break;
        default:
            break;
    }
}

static void collect_expr_reads(Emitter *e, const Expr *expr, SymSet *target, bool include_lambda_captures) {
    if (!expr) return;
    switch (expr->kind) {
        case EXPR_NAME:
            if (expr->u.name.symbol && expr->u.name.symbol->kind == SYM_VALUE) {
                symset_add(e->arena, target, expr->u.name.symbol);
            }
            break;
        case EXPR_UNARY:
            collect_expr_reads(e, expr->u.unary.operand, target, include_lambda_captures);
            break;
        case EXPR_CAST:
            collect_expr_reads(e, expr->u.cast.operand, target, include_lambda_captures);
            break;
        case EXPR_BINARY:
            collect_expr_reads(e, expr->u.binary.left, target, include_lambda_captures);
            collect_expr_reads(e, expr->u.binary.right, target, include_lambda_captures);
            break;
        case EXPR_COND:
            collect_expr_reads(e, expr->u.cond.cond, target, include_lambda_captures);
            collect_expr_reads(e, expr->u.cond.when_true, target, include_lambda_captures);
            collect_expr_reads(e, expr->u.cond.when_false, target, include_lambda_captures);
            break;
        case EXPR_CALL:
            if (include_lambda_captures && expr->u.call.symbol && expr->u.call.symbol->kind == SYM_LAMBDA) {
                const LiftedLambda *lifted = find_lifted(e, expr->u.call.symbol);
                if (lifted) {
                    for (int i = 0; i < lifted->captures.count; i++) {
                        symset_add(e->arena, target, lifted->captures.items[i]);
                    }
                }
            }
            for (int i = 0; i < expr->u.call.arg_count; i++) {
                collect_expr_reads(e, expr->u.call.args[i], target, include_lambda_captures);
            }
            break;
        default:
            break;
    }
}

static void collect_stmt_reads(Emitter *e, const Stmt *st);

static void collect_list_reads(Emitter *e, Stmt *const *stmts, int count) {
    for (int i = 0; i < count; i++) collect_stmt_reads(e, stmts[i]);
}

static void collect_stmt_reads(Emitter *e, const Stmt *st) {
    if (!st) return;
    switch (st->kind) {
        case STMT_BLOCK:
            collect_list_reads(e, st->u.block.stmts, st->u.block.count);
            break;
        case STMT_LET:
            if (st->u.let_s.init->kind != EXPR_LAMBDA) {
                collect_expr_reads(e, st->u.let_s.init, &e->read_values, true);
            }
            break;
        case STMT_DECL:
            for (int i = 0; i < st->u.decl.count; i++) {
                collect_expr_reads(e, st->u.decl.decls[i].init, &e->read_values, true);
            }
            break;
        case STMT_ASSIGN:
            if (st->u.assign.op != ASSIGN_SET && st->u.assign.symbol) {
                symset_add(e->arena, &e->read_values, st->u.assign.symbol);
            }
            if (st->u.assign.value) {
                collect_expr_reads(e, st->u.assign.value, &e->read_values, true);
            }
            break;
        case STMT_EXPR:
            collect_expr_reads(e, st->u.expr_s.expr, &e->read_values, true);
            break;
        case STMT_RETURN:
            collect_expr_reads(e, st->u.ret.value, &e->read_values, true);
            break;
        case STMT_IF:
            collect_expr_reads(e, st->u.if_s.cond, &e->read_values, true);
            collect_stmt_reads(e, st->u.if_s.then_branch);
            if (st->u.if_s.else_branch) collect_stmt_reads(e, st->u.if_s.else_branch);
            break;
        case STMT_WHILE:
            collect_expr_reads(e, st->u.while_s.cond, &e->read_values, true);
            collect_stmt_reads(e, st->u.while_s.body);
            break;
        case STMT_DO_WHILE:
            collect_stmt_reads(e, st->u.do_s.body);
            collect_expr_reads(e, st->u.do_s.cond, &e->read_values, true);
            break;
        case STMT_FOR:
            collect_list_reads(e, st->u.for_s.init, st->u.for_s.init_count);
            if (st->u.for_s.cond) collect_expr_reads(e, st->u.for_s.cond, &e->read_values, true);
            collect_stmt_reads(e, st->u.for_s.body);
            collect_list_reads(e, st->u.for_s.update, st->u.for_s.update_count);
            break;
        default:
            break;
    }
}

/* ---------------------------------------------------------------- expressions */

static bool is_atom(const Expr *expr) {
    return expr->kind == EXPR_INT_LIT || expr->kind == EXPR_BOOL_LIT || expr->kind == EXPR_NAME;
}

static bool may_have_effects(const Expr *expr) {
    switch (expr->kind) {
        case EXPR_CALL:
            return true;
        case EXPR_BINARY:
            if (expr->u.binary.op == BINOP_DIV || expr->u.binary.op == BINOP_REM) return true;
            return may_have_effects(expr->u.binary.left) || may_have_effects(expr->u.binary.right);
        case EXPR_UNARY:
            return may_have_effects(expr->u.unary.operand);
        case EXPR_CAST:
            return may_have_effects(expr->u.cast.operand);
        case EXPR_COND:
            return may_have_effects(expr->u.cond.cond)
                || may_have_effects(expr->u.cond.when_true)
                || may_have_effects(expr->u.cond.when_false);
        default:
            return false;
    }
}

static const char *new_temp(Emitter *e) {
    return arena_printf(e->arena, "fc_t%d", ++e->temp_counter);
}

static const char *emit_expr(Emitter *e, const Expr *expr, LineList *prep);
static const char *emit_unconverted(Emitter *e, const Expr *expr, LineList *prep);

static const char *emit_int_lit(Arena *a, int64_t val) {
    if (val == INT32_MIN) return "INT32_MIN";
    return arena_printf(a, "%" PRId64, val);
}

static const char *emit_converted(Emitter *e, const Expr *expr, FcType target, LineList *prep) {
    FcType src = expr->type;
    if (src == FC_TYPE_NONE || src == target) {
        return emit_unconverted(e, expr, prep);
    }

    if (expr->kind == EXPR_INT_LIT) {
        int64_t n = expr->u.int_val;
        switch (target) {
            case FC_TYPE_BOOL: return n != 0 ? "true" : "false";
            case FC_TYPE_CHAR: return arena_printf(e->arena, "((int8_t)%d)", (int)(int8_t)(uint8_t)(uint32_t)n);
            case FC_TYPE_UCHAR: return arena_printf(e->arena, "((uint8_t)%u)", (unsigned int)(uint8_t)(uint32_t)n);
            default: return emit_int_lit(e->arena, n);
        }
    }
    if (expr->kind == EXPR_BOOL_LIT) {
        bool b = expr->u.bool_val;
        switch (target) {
            case FC_TYPE_INT: return b ? "1" : "0";
            case FC_TYPE_CHAR: return b ? "((int8_t)1)" : "((int8_t)0)";
            case FC_TYPE_UCHAR: return b ? "((uint8_t)1)" : "((uint8_t)0)";
            default: return b ? "true" : "false";
        }
    }

    const char *code = emit_unconverted(e, expr, prep);
    if (src == FC_TYPE_BOOL && target == FC_TYPE_INT) return arena_printf(e->arena, "((int32_t)(%s))", code);
    if (src == FC_TYPE_BOOL && target == FC_TYPE_CHAR) return arena_printf(e->arena, "((int8_t)(%s))", code);
    if (src == FC_TYPE_BOOL && target == FC_TYPE_UCHAR) return arena_printf(e->arena, "((uint8_t)(%s))", code);
    if (target == FC_TYPE_BOOL) return arena_printf(e->arena, "(%s != 0)", code);
    if (target == FC_TYPE_INT) return arena_printf(e->arena, "((int32_t)(%s))", code);
    if (target == FC_TYPE_CHAR) {
        return src == FC_TYPE_INT
            ? arena_printf(e->arena, "fc_char(%s)", code)
            : arena_printf(e->arena, "fc_char((int32_t)(%s))", code);
    }
    if (target == FC_TYPE_UCHAR) {
        return src == FC_TYPE_INT
            ? arena_printf(e->arena, "fc_uchar(%s)", code)
            : arena_printf(e->arena, "fc_uchar((int32_t)(%s))", code);
    }
    return code;
}

static const char *emit_expr(Emitter *e, const Expr *expr, LineList *prep) {
    if (expr->convert_to != FC_TYPE_NONE) {
        return emit_converted(e, expr, expr->convert_to, prep);
    }
    return emit_unconverted(e, expr, prep);
}

static const char *emit_condition(Emitter *e, const Expr *cond, LineList *prep) {
    return emit_converted(e, cond, FC_TYPE_BOOL, prep);
}

static const char *emit_call(Emitter *e, const Expr *call, LineList *prep) {
    const char *callee;
    StrBuf argbuf;
    sb_init(&argbuf);
    bool first_arg = true;

    if (call->u.call.symbol->kind == SYM_LAMBDA) {
        const LiftedLambda *lifted = find_lifted(e, call->u.call.symbol);
        callee = lifted->c_func_name;
        for (int i = 0; i < lifted->captures.count; i++) {
            if (!first_arg) sb_append(&argbuf, ", ");
            sb_append(&argbuf, lifted->captures.items[i]->c_name);
            first_arg = false;
        }
    } else {
        callee = call->u.call.symbol->c_name;
    }

    int last_effect = -1;
    int effect_count = 0;
    for (int i = 0; i < call->u.call.arg_count; i++) {
        if (may_have_effects(call->u.call.args[i])) {
            effect_count++;
            last_effect = i;
        }
    }
    bool need_temps = (effect_count >= 2);

    for (int i = 0; i < call->u.call.arg_count; i++) {
        const Expr *arg = call->u.call.args[i];
        const char *text = emit_expr(e, arg, prep);
        if (need_temps && i < last_effect && !is_atom(arg)) {
            FcType t = (arg->convert_to != FC_TYPE_NONE) ? arg->convert_to : arg->type;
            const char *tmp = new_temp(e);
            ll_push(e->arena, prep, arena_printf(e->arena, "%s %s = %s;", c_type_name(t), tmp, text));
            text = tmp;
        }
        if (!first_arg) sb_append(&argbuf, ", ");
        sb_append(&argbuf, text);
        first_arg = false;
    }

    const char *res = arena_printf(e->arena, "%s(%s)", callee, argbuf.data ? argbuf.data : "");
    sb_free(&argbuf);
    return res;
}

static const char *emit_conditional(Emitter *e, const Expr *cond_expr, LineList *prep) {
    const char *cond = emit_condition(e, cond_expr->u.cond.cond, prep);
    LineList true_prep, false_prep;
    ll_init(&true_prep);
    ll_init(&false_prep);
    const char *wt = emit_expr(e, cond_expr->u.cond.when_true, &true_prep);
    const char *wf = emit_expr(e, cond_expr->u.cond.when_false, &false_prep);

    if (true_prep.count == 0 && false_prep.count == 0) {
        return arena_printf(e->arena, "(%s ? %s : %s)", cond, wt, wf);
    }

    const char *tmp = new_temp(e);
    ll_push(e->arena, prep, arena_printf(e->arena, "%s %s;", c_type_name(cond_expr->type), tmp));
    ll_push(e->arena, prep, arena_printf(e->arena, "if (%s) {", cond));
    ll_push_indented(e->arena, prep, &true_prep);
    ll_push(e->arena, prep, arena_printf(e->arena, "    %s = %s;", tmp, wt));
    ll_push(e->arena, prep, "} else {");
    ll_push_indented(e->arena, prep, &false_prep);
    ll_push(e->arena, prep, arena_printf(e->arena, "    %s = %s;", tmp, wf));
    ll_push(e->arena, prep, "}");
    return tmp;
}

static const char *emit_binary(Emitter *e, const Expr *bin, LineList *prep) {
    BinaryOp op = bin->u.binary.op;
    if (op == BINOP_AND || op == BINOP_OR) {
        const char *lc = emit_condition(e, bin->u.binary.left, prep);
        LineList rprep;
        ll_init(&rprep);
        const char *rc = emit_condition(e, bin->u.binary.right, &rprep);
        if (rprep.count == 0) {
            return arena_printf(e->arena, "(%s %s %s)", lc, binop_source(op), rc);
        }
        const char *tmp = new_temp(e);
        if (op == BINOP_AND) {
            ll_push(e->arena, prep, arena_printf(e->arena, "bool %s = false;", tmp));
            ll_push(e->arena, prep, arena_printf(e->arena, "if (%s) {", lc));
            ll_push_indented(e->arena, prep, &rprep);
            ll_push(e->arena, prep, arena_printf(e->arena, "    %s = %s;", tmp, rc));
            ll_push(e->arena, prep, "}");
        } else {
            ll_push(e->arena, prep, arena_printf(e->arena, "bool %s = true;", tmp));
            ll_push(e->arena, prep, arena_printf(e->arena, "if (!(%s)) {", lc));
            ll_push_indented(e->arena, prep, &rprep);
            ll_push(e->arena, prep, arena_printf(e->arena, "    %s = %s;", tmp, rc));
            ll_push(e->arena, prep, "}");
        }
        return tmp;
    }

    bool sequence = may_have_effects(bin->u.binary.left) && may_have_effects(bin->u.binary.right);
    const char *left = emit_expr(e, bin->u.binary.left, prep);
    if (sequence) {
        FcType lt = (bin->u.binary.left->convert_to != FC_TYPE_NONE)
            ? bin->u.binary.left->convert_to
            : bin->u.binary.left->type;
        const char *tmp = new_temp(e);
        ll_push(e->arena, prep, arena_printf(e->arena, "%s %s = %s;", c_type_name(lt), tmp, left));
        left = tmp;
    }
    const char *right = emit_expr(e, bin->u.binary.right, prep);

    switch (op) {
        case BINOP_ADD: return arena_printf(e->arena, "fc_add(%s, %s)", left, right);
        case BINOP_SUB: return arena_printf(e->arena, "fc_sub(%s, %s)", left, right);
        case BINOP_MUL: return arena_printf(e->arena, "fc_mul(%s, %s)", left, right);
        case BINOP_DIV: return arena_printf(e->arena, "fc_div(%s, %s)", left, right);
        case BINOP_REM: return arena_printf(e->arena, "fc_rem(%s, %s)", left, right);
        default: return arena_printf(e->arena, "(%s %s %s)", left, binop_source(op), right);
    }
}

static const char *emit_unconverted(Emitter *e, const Expr *expr, LineList *prep) {
    switch (expr->kind) {
        case EXPR_INT_LIT:
            return emit_int_lit(e->arena, expr->u.int_val);
        case EXPR_BOOL_LIT:
            return expr->u.bool_val ? "true" : "false";
        case EXPR_NAME:
            return expr->u.name.symbol->c_name;
        case EXPR_UNARY:
            if (expr->u.unary.op == UNOP_NEGATE) {
                return arena_printf(e->arena, "fc_neg(%s)", emit_expr(e, expr->u.unary.operand, prep));
            }
            if (expr->u.unary.operand->type == FC_TYPE_BOOL) {
                return arena_printf(e->arena, "(!(%s))", emit_expr(e, expr->u.unary.operand, prep));
            }
            return arena_printf(e->arena, "(%s == 0)", emit_expr(e, expr->u.unary.operand, prep));
        case EXPR_CAST:
            return emit_expr(e, expr->u.cast.operand, prep);
        case EXPR_BINARY:
            return emit_binary(e, expr, prep);
        case EXPR_COND:
            return emit_conditional(e, expr, prep);
        case EXPR_CALL:
            return emit_call(e, expr, prep);
        default:
            return "0";
    }
}

/* ---------------------------------------------------------------- statements */

static void emit_stmt(Emitter *e, const Stmt *st, LineList *lines);

static void emit_stmt_list(Emitter *e, Stmt *const *stmts, int count, LineList *lines) {
    for (int i = 0; i < count; i++) emit_stmt(e, stmts[i], lines);
}

static void emit_flattened(Emitter *e, const Stmt *st, LineList *lines) {
    if (st->kind == STMT_BLOCK) {
        emit_stmt_list(e, st->u.block.stmts, st->u.block.count, lines);
    } else {
        emit_stmt(e, st, lines);
    }
}

static const char *emit_assignment(Emitter *e, const Stmt *assign, LineList *prep) {
    const char *name = assign->u.assign.symbol->c_name;
    if (assign->u.assign.op == ASSIGN_SET) {
        return arena_printf(e->arena, "%s = %s", name, emit_expr(e, assign->u.assign.value, prep));
    }
    const char *helper;
    switch (assign->u.assign.op) {
        case ASSIGN_INC:
        case ASSIGN_ADD: helper = "fc_add"; break;
        case ASSIGN_DEC:
        case ASSIGN_SUB: helper = "fc_sub"; break;
        case ASSIGN_MUL: helper = "fc_mul"; break;
        case ASSIGN_DIV: helper = "fc_div"; break;
        case ASSIGN_REM: helper = "fc_rem"; break;
        default: helper = "fc_add"; break;
    }
    const char *operand = assign->u.assign.value ? emit_expr(e, assign->u.assign.value, prep) : "1";
    if (assign->u.assign.symbol->value_type == FC_TYPE_CHAR) {
        return arena_printf(e->arena, "%s = fc_char(%s((int32_t)%s, %s))", name, helper, name, operand);
    }
    if (assign->u.assign.symbol->value_type == FC_TYPE_UCHAR) {
        return arena_printf(e->arena, "%s = fc_uchar(%s((int32_t)%s, %s))", name, helper, name, operand);
    }
    return arena_printf(e->arena, "%s = %s(%s, %s)", name, helper, name, operand);
}

static void emit_if_tail(Emitter *e, const Stmt *branch, LineList *lines);

static void emit_if(Emitter *e, const Stmt *branch, LineList *lines) {
    const char *cond = emit_condition(e, branch->u.if_s.cond, lines);
    ll_push(e->arena, lines, arena_printf(e->arena, "if (%s) {", cond));
    emit_if_tail(e, branch, lines);
}

static void emit_if_tail(Emitter *e, const Stmt *branch, LineList *lines) {
    LineList then_lines;
    ll_init(&then_lines);
    emit_flattened(e, branch->u.if_s.then_branch, &then_lines);
    ll_push_indented(e->arena, lines, &then_lines);

    const Stmt *el = branch->u.if_s.else_branch;
    if (el && el->kind == STMT_IF) {
        LineList el_prep;
        ll_init(&el_prep);
        int saved_temp = e->temp_counter;
        const char *nc = emit_condition(e, el->u.if_s.cond, &el_prep);
        if (el_prep.count == 0) {
            ll_push(e->arena, lines, arena_printf(e->arena, "} else if (%s) {", nc));
            emit_if_tail(e, el, lines);
            return;
        }
        e->temp_counter = saved_temp;
        ll_push(e->arena, lines, "} else {");
        LineList nested;
        ll_init(&nested);
        emit_if(e, el, &nested);
        ll_push_indented(e->arena, lines, &nested);
        ll_push(e->arena, lines, "}");
        return;
    }
    if (el) {
        ll_push(e->arena, lines, "} else {");
        LineList else_lines;
        ll_init(&else_lines);
        emit_flattened(e, el, &else_lines);
        ll_push_indented(e->arena, lines, &else_lines);
        ll_push(e->arena, lines, "}");
    } else {
        ll_push(e->arena, lines, "}");
    }
}

static void emit_while(Emitter *e, const Stmt *loop, LineList *lines) {
    LineList cprep, body;
    ll_init(&cprep);
    ll_init(&body);
    const char *cond = emit_condition(e, loop->u.while_s.cond, &cprep);
    emit_flattened(e, loop->u.while_s.body, &body);

    if (cprep.count == 0) {
        ll_push(e->arena, lines, arena_printf(e->arena, "while (%s) {", cond));
        ll_push_indented(e->arena, lines, &body);
        ll_push(e->arena, lines, "}");
    } else {
        ll_push(e->arena, lines, "while (true) {");
        LineList combined;
        ll_init(&combined);
        ll_push_all(e->arena, &combined, &cprep);
        ll_push(e->arena, &combined, arena_printf(e->arena, "if (!(%s)) {", cond));
        ll_push(e->arena, &combined, "    break;");
        ll_push(e->arena, &combined, "}");
        ll_push_all(e->arena, &combined, &body);
        ll_push_indented(e->arena, lines, &combined);
        ll_push(e->arena, lines, "}");
    }
}

static void emit_do_while(Emitter *e, const Stmt *loop, LineList *lines) {
    LineList cprep, body;
    ll_init(&cprep);
    ll_init(&body);
    const char *cond = emit_condition(e, loop->u.do_s.cond, &cprep);
    emit_flattened(e, loop->u.do_s.body, &body);

    if (cprep.count == 0) {
        ll_push(e->arena, lines, "do {");
        ll_push_indented(e->arena, lines, &body);
        ll_push(e->arena, lines, arena_printf(e->arena, "} while (%s);", cond));
    } else {
        const char *flag = arena_printf(e->arena, "fc_first%d", ++e->loop_flag_counter);
        ll_push(e->arena, lines, arena_printf(e->arena, "for (bool %s = true; ; %s = false) {", flag, flag));
        LineList combined;
        ll_init(&combined);
        ll_push(e->arena, &combined, arena_printf(e->arena, "if (!%s) {", flag));
        ll_push_indented(e->arena, &combined, &cprep);
        ll_push(e->arena, &combined, arena_printf(e->arena, "    if (!(%s)) {", cond));
        ll_push(e->arena, &combined, "        break;");
        ll_push(e->arena, &combined, "    }");
        ll_push(e->arena, &combined, "}");
        ll_push_all(e->arena, &combined, &body);
        ll_push_indented(e->arena, lines, &combined);
        ll_push(e->arena, lines, "}");
    }
}

static bool try_format_simple_expr_list(Emitter *e, Stmt *const *stmts, int count, const char **out_text) {
    if (count == 0) {
        *out_text = "";
        return true;
    }
    int saved_temp = e->temp_counter;
    LineList prep;
    ll_init(&prep);
    StrBuf sb;
    sb_init(&sb);

    for (int i = 0; i < count; i++) {
        if (i > 0) sb_append(&sb, ", ");
        const Stmt *st = stmts[i];
        if (st->kind == STMT_ASSIGN) {
            sb_append(&sb, emit_assignment(e, st, &prep));
        } else if (st->kind == STMT_EXPR && st->u.expr_s.expr->kind == EXPR_CALL && st->u.expr_s.expr->u.call.is_print) {
            sb_printf(&sb, "fc_print(%s)", emit_expr(e, st->u.expr_s.expr->u.call.args[0], &prep));
        } else if (st->kind == STMT_EXPR && st->u.expr_s.expr->kind == EXPR_CALL) {
            sb_printf(&sb, "(void)%s", emit_call(e, st->u.expr_s.expr, &prep));
        } else {
            sb_free(&sb);
            e->temp_counter = saved_temp;
            return false;
        }
    }

    if (prep.count > 0) {
        sb_free(&sb);
        e->temp_counter = saved_temp;
        return false;
    }
    *out_text = arena_strndup(e->arena, sb.data, sb.len);
    sb_free(&sb);
    return true;
}

static bool try_format_simple_for_init(Emitter *e, Stmt *const *init, int count, const char **out_text) {
    if (count == 0) {
        *out_text = "";
        return true;
    }
    if (count == 1 && init[0]->kind == STMT_DECL) {
        const Stmt *decl = init[0];
        for (int i = 0; i < decl->u.decl.count; i++) {
            if (!symset_contains(&e->read_values, decl->u.decl.decls[i].symbol)) return false;
        }
        int saved_temp = e->temp_counter;
        LineList prep;
        ll_init(&prep);
        StrBuf sb;
        sb_init(&sb);
        sb_printf(&sb, "%s ", c_type_name(decl->u.decl.type));
        for (int i = 0; i < decl->u.decl.count; i++) {
            if (i > 0) sb_append(&sb, ", ");
            const VarDeclarator *d = &decl->u.decl.decls[i];
            sb_printf(&sb, "%s = %s", d->symbol->c_name, emit_expr(e, d->init, &prep));
        }
        if (prep.count > 0) {
            sb_free(&sb);
            e->temp_counter = saved_temp;
            return false;
        }
        *out_text = arena_strndup(e->arena, sb.data, sb.len);
        sb_free(&sb);
        return true;
    }
    if (count == 1 && init[0]->kind == STMT_LET && init[0]->u.let_s.init->kind != EXPR_LAMBDA) {
        const Symbol *sym = init[0]->u.let_s.symbol;
        if (!symset_contains(&e->read_values, sym)) return false;
        int saved_temp = e->temp_counter;
        LineList prep;
        ll_init(&prep);
        const char *expr = emit_expr(e, init[0]->u.let_s.init, &prep);
        if (prep.count > 0) {
            e->temp_counter = saved_temp;
            return false;
        }
        *out_text = arena_printf(e->arena, "const %s %s = %s", c_type_name(sym->value_type), sym->c_name, expr);
        return true;
    }
    return try_format_simple_expr_list(e, init, count, out_text);
}

static void emit_for(Emitter *e, const Stmt *loop, LineList *lines) {
    LineList cprep;
    ll_init(&cprep);
    const char *cond_text = loop->u.for_s.cond ? emit_condition(e, loop->u.for_s.cond, &cprep) : "";

    const char *init_text = NULL;
    const char *update_text = NULL;
    if (cprep.count == 0
        && try_format_simple_for_init(e, loop->u.for_s.init, loop->u.for_s.init_count, &init_text)
        && try_format_simple_expr_list(e, loop->u.for_s.update, loop->u.for_s.update_count, &update_text))
    {
        if (init_text[0] == '\0' && cond_text[0] == '\0' && update_text[0] == '\0') {
            ll_push(e->arena, lines, "for (;;) {");
        } else {
            ll_push(e->arena, lines, arena_printf(e->arena, "for (%s; %s; %s) {", init_text, cond_text, update_text));
        }
        LineList body;
        ll_init(&body);
        emit_flattened(e, loop->u.for_s.body, &body);
        ll_push_indented(e->arena, lines, &body);
        ll_push(e->arena, lines, "}");
        return;
    }

    ll_push(e->arena, lines, "{");
    LineList block_lines, body;
    ll_init(&block_lines);
    ll_init(&body);
    emit_stmt_list(e, loop->u.for_s.init, loop->u.for_s.init_count, &block_lines);
    emit_flattened(e, loop->u.for_s.body, &body);

    if (loop->u.for_s.update_count > 0) {
        const char *flag = arena_printf(e->arena, "fc_first%d", ++e->loop_flag_counter);
        ll_push(e->arena, &block_lines, arena_printf(e->arena, "for (bool %s = true; ; %s = false) {", flag, flag));
        LineList inner, upd;
        ll_init(&inner);
        ll_init(&upd);
        ll_push(e->arena, &inner, arena_printf(e->arena, "if (!%s) {", flag));
        emit_stmt_list(e, loop->u.for_s.update, loop->u.for_s.update_count, &upd);
        ll_push_indented(e->arena, &inner, &upd);
        ll_push(e->arena, &inner, "}");
        if (loop->u.for_s.cond) {
            ll_push_all(e->arena, &inner, &cprep);
            ll_push(e->arena, &inner, arena_printf(e->arena, "if (!(%s)) {", cond_text));
            ll_push(e->arena, &inner, "    break;");
            ll_push(e->arena, &inner, "}");
        }
        ll_push_all(e->arena, &inner, &body);
        ll_push_indented(e->arena, &block_lines, &inner);
        ll_push(e->arena, &block_lines, "}");
    } else if (cprep.count == 0) {
        if (!loop->u.for_s.cond) {
            ll_push(e->arena, &block_lines, "for (;;) {");
        } else {
            ll_push(e->arena, &block_lines, arena_printf(e->arena, "while (%s) {", cond_text));
        }
        ll_push_indented(e->arena, &block_lines, &body);
        ll_push(e->arena, &block_lines, "}");
    } else {
        ll_push(e->arena, &block_lines, "while (true) {");
        LineList inner;
        ll_init(&inner);
        ll_push_all(e->arena, &inner, &cprep);
        ll_push(e->arena, &inner, arena_printf(e->arena, "if (!(%s)) {", cond_text));
        ll_push(e->arena, &inner, "    break;");
        ll_push(e->arena, &inner, "}");
        ll_push_all(e->arena, &inner, &body);
        ll_push_indented(e->arena, &block_lines, &inner);
        ll_push(e->arena, &block_lines, "}");
    }

    ll_push_indented(e->arena, lines, &block_lines);
    ll_push(e->arena, lines, "}");
}

static void emit_stmt(Emitter *e, const Stmt *st, LineList *lines) {
    switch (st->kind) {
        case STMT_EMPTY:
            break;
        case STMT_BLOCK: {
            ll_push(e->arena, lines, "{");
            LineList inner;
            ll_init(&inner);
            emit_stmt_list(e, st->u.block.stmts, st->u.block.count, &inner);
            ll_push_indented(e->arena, lines, &inner);
            ll_push(e->arena, lines, "}");
            break;
        }
        case STMT_LET: {
            if (st->u.let_s.init->kind == EXPR_LAMBDA) break;
            const Symbol *sym = st->u.let_s.symbol;
            const char *init = emit_expr(e, st->u.let_s.init, lines);
            ll_push(e->arena, lines, arena_printf(e->arena, "const %s %s = %s;", c_type_name(sym->value_type), sym->c_name, init));
            if (!symset_contains(&e->read_values, sym)) {
                ll_push(e->arena, lines, arena_printf(e->arena, "(void)%s;", sym->c_name));
            }
            break;
        }
        case STMT_DECL: {
            const char *tname = c_type_name(st->u.decl.type);
            for (int i = 0; i < st->u.decl.count; i++) {
                const VarDeclarator *d = &st->u.decl.decls[i];
                const char *init = emit_expr(e, d->init, lines);
                ll_push(e->arena, lines, arena_printf(e->arena, "%s %s = %s;", tname, d->symbol->c_name, init));
                if (!symset_contains(&e->read_values, d->symbol)) {
                    ll_push(e->arena, lines, arena_printf(e->arena, "(void)%s;", d->symbol->c_name));
                }
            }
            break;
        }
        case STMT_ASSIGN: {
            const char *txt = emit_assignment(e, st, lines);
            ll_push(e->arena, lines, arena_printf(e->arena, "%s;", txt));
            break;
        }
        case STMT_EXPR: {
            const Expr *ex = st->u.expr_s.expr;
            if (ex->kind == EXPR_CALL && ex->u.call.is_print) {
                const char *arg = emit_expr(e, ex->u.call.args[0], lines);
                ll_push(e->arena, lines, arena_printf(e->arena, "fc_print(%s);", arg));
            } else if (ex->kind == EXPR_CALL) {
                const char *ct = emit_call(e, ex, lines);
                ll_push(e->arena, lines, arena_printf(e->arena, "(void)%s;", ct));
            }
            break;
        }
        case STMT_RETURN: {
            const char *val = emit_expr(e, st->u.ret.value, lines);
            ll_push(e->arena, lines, arena_printf(e->arena, "return %s;", val));
            break;
        }
        case STMT_BREAK:
            ll_push(e->arena, lines, "break;");
            break;
        case STMT_CONTINUE:
            ll_push(e->arena, lines, "continue;");
            break;
        case STMT_IF:
            emit_if(e, st, lines);
            break;
        case STMT_WHILE:
            emit_while(e, st, lines);
            break;
        case STMT_DO_WHILE:
            emit_do_while(e, st, lines);
            break;
        case STMT_FOR:
            emit_for(e, st, lines);
            break;
    }
}

/* ---------------------------------------------------------------- functions & top-level */

static void emit_lifted_lambda(Emitter *e, const LiftedLambda *lifted) {
    StrBuf params;
    sb_init(&params);
    bool first = true;
    for (int i = 0; i < lifted->captures.count; i++) {
        const Symbol *cap = lifted->captures.items[i];
        if (!first) sb_append(&params, ", ");
        sb_printf(&params, "const %s %s", c_type_name(cap->value_type), cap->c_name);
        first = false;
    }
    for (int i = 0; i < lifted->expr->u.lambda.param_count; i++) {
        const Parameter *p = &lifted->expr->u.lambda.params[i];
        if (!first) sb_append(&params, ", ");
        sb_printf(&params, "const %s %s", c_type_name(p->type), p->symbol->c_name);
        first = false;
    }
    if (first) sb_append(&params, "void");

    sb_printf(e->out, "static inline %s %s(%s)\n{\n",
        c_type_name(lifted->symbol->return_type), lifted->c_func_name, params.data);
    sb_free(&params);

    SymSet local_reads;
    symset_init(&local_reads);
    collect_expr_reads(e, lifted->expr->u.lambda.body, &local_reads, false);

    LineList body;
    ll_init(&body);
    for (int i = 0; i < lifted->expr->u.lambda.param_count; i++) {
        const Symbol *ps = lifted->expr->u.lambda.params[i].symbol;
        if (!symset_contains(&local_reads, ps)) {
            ll_push(e->arena, &body, arena_printf(e->arena, "(void)%s;", ps->c_name));
        }
    }
    const char *val = emit_expr(e, lifted->expr->u.lambda.body, &body);
    ll_push(e->arena, &body, arena_printf(e->arena, "return %s;", val));

    for (int i = 0; i < body.count; i++) {
        sb_printf(e->out, "    %s\n", body.items[i]);
    }
    sb_append(e->out, "}\n\n");
}

static void emit_function(Emitter *e, const FunctionDecl *fn) {
    e->lambdas = NULL;
    e->lambda_count = 0;
    e->lambda_cap = 0;
    symset_init(&e->read_values);
    e->temp_counter = 0;
    e->loop_flag_counter = 0;

    bool calls_enclosing = false;
    collect_lambdas_list(e, fn, fn->body, fn->body_count, &calls_enclosing);
    collect_list_reads(e, fn->body, fn->body_count);

    const char *hdr = func_header(e->arena, fn);
    if (calls_enclosing) {
        sb_printf(e->out, "%s;\n\n", hdr);
    }

    for (int i = 0; i < e->lambda_count; i++) {
        emit_lifted_lambda(e, &e->lambdas[i]);
    }

    sb_printf(e->out, "%s\n{\n", hdr);
    LineList body;
    ll_init(&body);
    for (int i = 0; i < fn->param_count; i++) {
        const Symbol *ps = fn->params[i].symbol;
        if (!symset_contains(&e->read_values, ps)) {
            ll_push(e->arena, &body, arena_printf(e->arena, "(void)%s;", ps->c_name));
        }
    }
    emit_stmt_list(e, fn->body, fn->body_count, &body);
    for (int i = 0; i < body.count; i++) {
        sb_printf(e->out, "    %s\n", body.items[i]);
    }
    sb_append(e->out, "}\n\n");
}

void fc_emit_c(Arena *a, const ProgramNode *program, const char *source_name, StrBuf *out) {
    Emitter e;
    memset(&e, 0, sizeof(e));
    e.arena = a;
    e.out = out;

    sb_line(out, "/*");
    sb_printf(out, " * Generated by fc2c from %s. Do not edit; edit the .fc source and translate again.\n", source_name);
    sb_line(out, " */");
    sb_line(out, "#include <inttypes.h>");
    sb_line(out, "#include <stdbool.h>");
    sb_line(out, "#include <stdint.h>");
    sb_line(out, "#include <stdio.h>");
    sb_line(out, "#include <stdlib.h>");
    sb_line(out, "#ifdef _WIN32");
    sb_line(out, "#include <fcntl.h>");
    sb_line(out, "#include <io.h>");
    sb_line(out, "#endif");
    sb_line(out, "");
    sb_line(out, "/* Runtime support. User identifiers use the 'u_' prefix and internal names use 'fc_',");
    sb_line(out, " * so they never collide with each other, C keywords, macros, or standard library names. */");
    sb_line(out, "");
    sb_line(out, "/* print(int): writes the decimal value and a \"\\n\" newline to standard output. */");
    sb_line(out, "static inline void fc_print(int32_t value)");
    sb_line(out, "{");
    sb_line(out, "    printf(\"%\" PRId32 \"\\n\", value);");
    sb_line(out, "}");
    sb_line(out, "");
    sb_line(out, "/* Two's-complement 32-bit wraparound arithmetic without C signed-overflow undefined behavior. */");
    sb_line(out, "static inline int32_t fc_add(int32_t a, int32_t b)");
    sb_line(out, "{");
    sb_line(out, "    return (int32_t)((uint32_t)a + (uint32_t)b);");
    sb_line(out, "}");
    sb_line(out, "");
    sb_line(out, "static inline int32_t fc_sub(int32_t a, int32_t b)");
    sb_line(out, "{");
    sb_line(out, "    return (int32_t)((uint32_t)a - (uint32_t)b);");
    sb_line(out, "}");
    sb_line(out, "");
    sb_line(out, "static inline int32_t fc_mul(int32_t a, int32_t b)");
    sb_line(out, "{");
    sb_line(out, "    return (int32_t)((uint32_t)a * (uint32_t)b);");
    sb_line(out, "}");
    sb_line(out, "");
    sb_line(out, "static inline int32_t fc_neg(int32_t a)");
    sb_line(out, "{");
    sb_line(out, "    return (int32_t)(0u - (uint32_t)a);");
    sb_line(out, "}");
    sb_line(out, "");
    sb_line(out, "/* Checked division and remainder: avoids C undefined behavior on /0 and INT32_MIN / -1. */");
    sb_line(out, "static inline int32_t fc_div(int32_t a, int32_t b)");
    sb_line(out, "{");
    sb_line(out, "    if (b == 0) {");
    sb_line(out, "        fprintf(stderr, \"FunctionalC runtime error: division by zero\\n\");");
    sb_line(out, "        exit(1);");
    sb_line(out, "    }");
    sb_line(out, "    if (a == INT32_MIN && b == -1) {");
    sb_line(out, "        fprintf(stderr, \"FunctionalC runtime error: arithmetic overflow in division\\n\");");
    sb_line(out, "        exit(1);");
    sb_line(out, "    }");
    sb_line(out, "    return a / b;");
    sb_line(out, "}");
    sb_line(out, "");
    sb_line(out, "static inline int32_t fc_rem(int32_t a, int32_t b)");
    sb_line(out, "{");
    sb_line(out, "    if (b == 0) {");
    sb_line(out, "        fprintf(stderr, \"FunctionalC runtime error: division by zero\\n\");");
    sb_line(out, "        exit(1);");
    sb_line(out, "    }");
    sb_line(out, "    if (a == INT32_MIN && b == -1) {");
    sb_line(out, "        fprintf(stderr, \"FunctionalC runtime error: arithmetic overflow in remainder\\n\");");
    sb_line(out, "        exit(1);");
    sb_line(out, "    }");
    sb_line(out, "    return a % b;");
    sb_line(out, "}");
    sb_line(out, "");
    sb_line(out, "/* Narrowing to 8-bit integer types keeps the low 8 bits (two's complement). */");
    sb_line(out, "static inline int8_t fc_char(int32_t value)");
    sb_line(out, "{");
    sb_line(out, "    return (int8_t)(uint8_t)(uint32_t)value;");
    sb_line(out, "}");
    sb_line(out, "");
    sb_line(out, "static inline uint8_t fc_uchar(int32_t value)");
    sb_line(out, "{");
    sb_line(out, "    return (uint8_t)(uint32_t)value;");
    sb_line(out, "}");
    sb_line(out, "");
    sb_line(out, "/* Program */");
    sb_line(out, "");

    const FunctionDecl *main_fn = NULL;
    for (int i = 0; i < program->func_count; i++) {
        emit_function(&e, program->functions[i]);
        if (!main_fn && strcmp(program->functions[i]->name, "main") == 0) {
            main_fn = program->functions[i];
        }
    }

    sb_line(out, "/* C entry point: configures binary stdout on Windows (for \"\\n\" line endings),");
    sb_line(out, " * calls the source-level main(), and returns its result as the process exit code. */");
    sb_line(out, "int main(void)");
    sb_line(out, "{");
    sb_line(out, "#ifdef _WIN32");
    sb_line(out, "    _setmode(_fileno(stdout), _O_BINARY);");
    sb_line(out, "#endif");
    sb_printf(out, "    return (int)%s();\n", main_fn->symbol->c_name);
    sb_line(out, "}");
}
