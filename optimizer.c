/*
 * optimizer.c - rule based IR optimizer
 */
#include "optimizer.h"
#include "eval.h"
#include "symbol_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

int g_opt_verbose = 1;
static int nrewrites = 0;

static void logr(const char *rule, const char *fmt, ...) {
    nrewrites++;
    if (!g_opt_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("  %s[%s]%s %s\n", COL_MAGENTA, rule, COL_RESET, buf);
}
static void info(const char *rule, const char *fmt, ...) {
    if (!g_opt_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("  %s[%s]%s %s\n", COL_CYAN, rule, COL_RESET, buf);
}

/* ======================================================================
 *  1. Constant folding
 * ====================================================================== */
static int is_lit(Node *e) {
    return e && (e->type == N_INT || e->type == N_FLOAT || e->type == N_STRING || e->type == N_DATE ||
                 e->type == N_NULL || e->type == N_BOOL);
}
static int is_bool_lit(Node *e, int v) { return e && e->type == N_BOOL && e->ival == v; }

static Node *lit_from_value(Value v, Node *orig) {
    Node *n;
    switch (v.k) {
    case V_INT: n = mk_int(v.i); break;
    case V_FLOAT: n = mk_float(v.f); break;
    case V_BOOL: n = mk_bool((int)v.i); break;
    case V_STR:
        n = mk_string(v.s);
        if (orig->dtype == DT_DATE) n->type = N_DATE;
        break;
    default: n = mk_null(); break;
    }
    n->line = orig->line;
    n->col = orig->col;
    if (v.k != V_NULL && orig->dtype != DT_UNKNOWN) n->dtype = orig->dtype;
    return n;
}

static int all_kids_literal(Node *e) {
    for (int i = 0; i < e->nkids; i++) {
        Node *k = e->kids[i];
        if (k && k->type == N_LIST) { if (!all_kids_literal(k)) return 0; }
        else if (!is_lit(k)) return 0;
    }
    return 1;
}

static Node *fold(Node *e, int *changed) {
    if (!e || e->slot >= 0) return e;
    if (e->type == N_QUERY || e->type == N_SUBQUERY || e->type == N_EXISTS || e->type == N_AGG) return e;
    if (e->type == N_IN_SUBQ) { e->kids[0] = fold(e->kids[0], changed); return e; }
    for (int i = 0; i < e->nkids; i++) e->kids[i] = fold(e->kids[i], changed);
    if (e->type == N_LIST || is_lit(e)) return e;

    /* boolean simplifications */
    if (e->type == N_BINOP && e->op == O_AND) {
        Node *a = e->kids[0], *b = e->kids[1];
        if (is_bool_lit(a, 0) || is_bool_lit(b, 0)) { *changed = 1; Node *f = mk_bool(0); f->line = e->line; f->col = e->col; return f; }
        if (is_bool_lit(a, 1)) { *changed = 1; return b; }
        if (is_bool_lit(b, 1)) { *changed = 1; return a; }
    }
    if (e->type == N_BINOP && e->op == O_OR) {
        Node *a = e->kids[0], *b = e->kids[1];
        if (is_bool_lit(a, 1) || is_bool_lit(b, 1)) { *changed = 1; Node *t = mk_bool(1); t->line = e->line; t->col = e->col; return t; }
        if (is_bool_lit(a, 0)) { *changed = 1; return b; }
        if (is_bool_lit(b, 0)) { *changed = 1; return a; }
    }
    if (e->type == N_UNOP && e->op == O_NOT && e->kids[0]->type == N_UNOP && e->kids[0]->op == O_NOT) {
        *changed = 1;
        return e->kids[0]->kids[0];
    }

    int foldable = 0;
    switch (e->type) {
    case N_BINOP: case N_UNOP: case N_BETWEEN: case N_IN_LIST: case N_LIKE: case N_IS_NULL: case N_FUNC:
        foldable = all_kids_literal(e);
        break;
    default: break;
    }
    if (!foldable) return e;
    Node *volatile orig = e;
    jmp_buf jb;
    jmp_buf *saved = g_exec_jmp;
    g_exec_jmp = &jb;
    if (setjmp(jb)) { g_exec_jmp = saved; return orig; }   /* e.g. division by zero: leave for run time */
    Value v = eval_expr(orig, NULL);
    g_exec_jmp = saved;
    Node *r = lit_from_value(v, orig);
    v_free(&v);
    *changed = 1;
    return r;
}

static void fold_slot(Node **slot, const char *where) {
    if (!*slot) return;
    int changed = 0;
    char *before = expr_to_sql(*slot);
    *slot = fold(*slot, &changed);
    if (changed) {
        char *after = expr_to_sql(*slot);
        logr("CONSTANT FOLDING", "%s: %s  =>  %s", where, before, after);
        free(after);
    }
    free(before);
}

static void fold_tree(IRNode *n) {
    if (!n) return;
    fold_tree(n->left);
    fold_tree(n->right);
    fold_tree(n->sub);
    switch (n->op) {
    case IR_FILTER: fold_slot(&n->pred, n->filter_kind == FK_HAVING ? "HAVING" : "WHERE"); break;
    case IR_JOIN: fold_slot(&n->pred, "JOIN condition"); break;
    case IR_PROJECT:
        for (int i = 0; i < n->nexprs; i++) fold_slot(&n->exprs[i], "SELECT expression");
        break;
    case IR_VALUES:
        for (int r = 0; r < n->ast->nkids; r++)
            for (int i = 0; i < n->ast->kids[r]->nkids; i++) fold_slot(&n->ast->kids[r]->kids[i], "VALUES");
        break;
    case IR_UPDATE:
        for (int i = 0; i < n->ast->kids[0]->nkids; i++) fold_slot(&n->ast->kids[0]->kids[i]->kids[0], "SET expression");
        break;
    default: break;
    }
}

/* ======================================================================
 *  2. Redundant filter removal
 * ====================================================================== */
static IRNode *drop_true_filters(IRNode *n) {
    if (!n) return n;
    n->left = drop_true_filters(n->left);
    n->right = drop_true_filters(n->right);
    n->sub = drop_true_filters(n->sub);
    if (n->op == IR_FILTER && is_bool_lit(n->pred, 1)) {
        logr("REDUNDANT FILTER", "condition is always TRUE - FILTER removed");
        return n->left;
    }
    if (n->op == IR_FILTER && (is_bool_lit(n->pred, 0) || (n->pred && n->pred->type == N_NULL))) {
        n->note = xstrdup("condition is always FALSE: produces no rows");
        info("CONTRADICTION", "WHERE condition is always FALSE - the result will be empty");
    }
    if (n->op == IR_JOIN && n->pred && is_bool_lit(n->pred, 1) && n->join_type == J_INNER) {
        n->join_type = J_CROSS;
        n->pred = NULL;
        logr("REDUNDANT FILTER", "JOIN condition is always TRUE - join becomes a CROSS JOIN");
    }
    return n;
}

/* ======================================================================
 *  Helpers: which ranges (tables / aliases) an IR subtree or an
 *  expression refers to
 * ====================================================================== */
static void quals_of(IRNode *n, PtrVec *v) {
    if (!n) return;
    switch (n->op) {
    case IR_LOAD: pv_push(v, n->alias ? n->alias : n->table); break;
    case IR_VIEW: case IR_DERIVED: pv_push(v, n->alias); break;
    case IR_JOIN: quals_of(n->left, v); quals_of(n->right, v); break;
    default: quals_of(n->left, v);
    }
}
static int has_ci(PtrVec *v, const char *s) {
    for (int i = 0; i < v->n; i++) if (ci_eq(v->items[i], s)) return 1;
    return 0;
}
static int subset(PtrVec *a, PtrVec *b) {
    for (int i = 0; i < a->n; i++) if (!has_ci(b, a->items[i])) return 0;
    return 1;
}
/* collect range names of column references that belong to the query at `level` */
static void expr_quals(Node *e, int level, PtrVec *v) {
    if (!e) return;
    if (e->type == N_COLREF) {
        if (e->depth == level && e->rqual && !has_ci(v, e->rqual)) pv_push(v, e->rqual);
        return;
    }
    if (e->type == N_QUERY) {
        for (int i = 0; i < e->nkids; i++) expr_quals(e->kids[i], level + 1, v);
        return;
    }
    for (int i = 0; i < e->nkids; i++) expr_quals(e->kids[i], level, v);
}
static void split_and(Node *e, PtrVec *out) {
    if (e && e->type == N_BINOP && e->op == O_AND) { split_and(e->kids[0], out); split_and(e->kids[1], out); }
    else if (e) pv_push(out, e);
}
static Node *join_and(Node **v, int n) {
    if (n == 0) return NULL;
    Node *r = v[0];
    for (int i = 1; i < n; i++) { r = mk_binop(O_AND, r, v[i]); r->dtype = DT_BOOLEAN; }
    return r;
}
static char *vec_str(PtrVec *v) {
    SB b; sb_init(&b);
    for (int i = 0; i < v->n; i++) sb_add(&b, "%s%s", i ? ", " : "", (char *)v->items[i]);
    return sb_take(&b);
}
static int contains_join(IRNode *n) {
    if (!n) return 0;
    if (n->op == IR_JOIN) return 1;
    if (n->op == IR_VIEW || n->op == IR_DERIVED) return 0;
    return contains_join(n->left) || contains_join(n->right);
}

/* ======================================================================
 *  3. Selection pushdown
 * ====================================================================== */
static int push_conj(IRNode **slot, Node *c, PtrVec *cq, char **target) {
    IRNode *n = *slot;
    if (n->op == IR_JOIN) {
        PtrVec L = {0}, R = {0};
        quals_of(n->left, &L);
        quals_of(n->right, &R);
        int jt = n->join_type, r = 0;
        if (subset(cq, &L) && (jt == J_INNER || jt == J_CROSS || jt == J_LEFT)) r = push_conj(&n->left, c, cq, target);
        else if (subset(cq, &R) && (jt == J_INNER || jt == J_CROSS || jt == J_RIGHT)) r = push_conj(&n->right, c, cq, target);
        else if (jt == J_INNER || jt == J_CROSS) {
            Node *v[2] = { n->pred, c };
            n->pred = n->pred ? join_and(v, 2) : c;
            if (jt == J_CROSS) {
                n->join_type = J_INNER;
                char *s = expr_to_sql(c);
                logr("JOIN REWRITE", "CROSS JOIN + WHERE %s  =>  INNER JOIN ... ON %s", s, s);
                free(s);
            }
            *target = xstrdup("the JOIN condition");
            r = 1;
        }
        free(L.items);
        free(R.items);
        return r;
    }
    if (n->op == IR_FILTER && n->filter_kind == FK_PUSHED) {
        Node *v[2] = { n->pred, c };
        n->pred = join_and(v, 2);
        PtrVec q = {0};
        quals_of(n, &q);
        *target = vec_str(&q);
        free(q.items);
        return 1;
    }
    if (n->op == IR_LOAD || n->op == IR_VIEW || n->op == IR_DERIVED) {
        IRNode *f = ir_new(IR_FILTER);
        f->left = n;
        f->pred = c;
        f->filter_kind = FK_PUSHED;
        *slot = f;
        *target = xstrdup(n->alias ? n->alias : n->table);
        return 1;
    }
    return 0;
}

static void push_on_conditions(IRNode *n) {
    if (!n || n->op != IR_JOIN) return;
    push_on_conditions(n->left);
    push_on_conditions(n->right);
    int jt = n->join_type;
    if (!n->pred || jt == J_FULL || jt == J_CROSS) return;
    PtrVec conj = {0}, keep = {0}, L = {0}, R = {0};
    split_and(n->pred, &conj);
    quals_of(n->left, &L);
    quals_of(n->right, &R);
    for (int i = 0; i < conj.n; i++) {
        Node *c = conj.items[i];
        PtrVec cq = {0};
        expr_quals(c, 0, &cq);
        char *target = NULL;
        int pushed = 0;
        if (cq.n && subset(&cq, &L) && (jt == J_INNER || jt == J_RIGHT)) pushed = push_conj(&n->left, c, &cq, &target);
        else if (cq.n && subset(&cq, &R) && (jt == J_INNER || jt == J_LEFT)) pushed = push_conj(&n->right, c, &cq, &target);
        if (pushed) {
            char *s = expr_to_sql(c);
            logr("SELECTION PUSHDOWN", "ON-condition %s moved below the join onto %s", s, target);
            free(s);
        } else pv_push(&keep, c);
        free(target);
        free(cq.items);
    }
    n->pred = join_and((Node **)keep.items, keep.n);
    if (!n->pred && jt == J_INNER) n->join_type = J_CROSS;
    free(conj.items); free(keep.items); free(L.items); free(R.items);
}

static IRNode *pushdown(IRNode *n) {
    if (!n) return n;
    n->left = pushdown(n->left);
    n->right = pushdown(n->right);
    if (n->op == IR_FILTER && n->filter_kind == FK_WHERE && contains_join(n->left)) {
        PtrVec conj = {0}, keep = {0};
        split_and(n->pred, &conj);
        for (int i = 0; i < conj.n; i++) {
            Node *c = conj.items[i];
            PtrVec cq = {0};
            expr_quals(c, 0, &cq);
            char *target = NULL;
            if (cq.n && push_conj(&n->left, c, &cq, &target)) {
                char *s = expr_to_sql(c);
                if (!strcmp(target, "the JOIN condition"))
                    logr("SELECTION PUSHDOWN", "WHERE %s merged into the JOIN condition (filters while joining)", s);
                else
                    logr("SELECTION PUSHDOWN", "WHERE %s now runs before the join, directly on %s", s, target);
                free(s);
            } else pv_push(&keep, c);
            free(target);
            free(cq.items);
        }
        free(conj.items);
        if (keep.n == 0) {
            logr("SELECTION PUSHDOWN", "the original WHERE FILTER is now empty and was removed");
            free(keep.items);
            return n->left;
        }
        n->pred = join_and((Node **)keep.items, keep.n);
        free(keep.items);
    }
    return n;
}

/* ======================================================================
 *  Join algorithm selection: an INNER equi-join uses a hash join
 * ====================================================================== */
static void choose_join_algorithms(IRNode *n) {
    if (!n) return;
    choose_join_algorithms(n->left);
    choose_join_algorithms(n->right);
    if (n->op != IR_JOIN) return;
    if (n->join_type == J_CROSS || !n->pred) { n->note = xstrdup("nested-loop (cartesian product)"); return; }
    if (n->join_type == J_INNER || n->join_type == J_LEFT) {
        PtrVec conj = {0}, L = {0}, R = {0};
        split_and(n->pred, &conj);
        quals_of(n->left, &L);
        quals_of(n->right, &R);
        for (int i = 0; i < conj.n && !n->hkey_l; i++) {
            Node *c = conj.items[i];
            if (c->type != N_BINOP || c->op != O_EQ) continue;
            Node *a = c->kids[0], *b = c->kids[1];
            if (a->type != N_COLREF || b->type != N_COLREF || a->depth || b->depth) continue;
            if (has_ci(&L, a->rqual) && has_ci(&R, b->rqual)) { n->hkey_l = a; n->hkey_r = b; }
            else if (has_ci(&L, b->rqual) && has_ci(&R, a->rqual)) { n->hkey_l = b; n->hkey_r = a; }
        }
        if (n->hkey_l) {
            char *x = expr_to_sql(n->hkey_l), *y = expr_to_sql(n->hkey_r);
            n->note = xsprintf("hash join on %s = %s", x, y);
            logr("JOIN ALGORITHM", "equi-join %s = %s -> HASH JOIN (build on right input, probe with left)", x, y);
            free(x); free(y);
        } else n->note = xstrdup("nested-loop join (no equality condition)");
        free(conj.items); free(L.items); free(R.items);
    } else n->note = xstrdup("nested-loop join");
}

/* ======================================================================
 *  4. Projection reduction (column pruning)
 * ====================================================================== */
static void need_add(PtrVec *need, const char *q, const char *c) {
    char *k = xsprintf("%s.%s", q, c);
    for (char *p = k; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (!pv_contains_str(need, k)) pv_push(need, k); else free(k);
}
static void expr_need(Node *e, int level, PtrVec *need) {
    if (!e) return;
    if (e->type == N_COLREF) {
        if (e->depth == level && e->rqual) need_add(need, e->rqual, e->s2);
        return;
    }
    if (e->type == N_QUERY) {
        QInfo *qi = e->info;
        for (int i = 0; i < e->nkids; i++) expr_need(e->kids[i], level + 1, need);
        (void)qi;
        return;
    }
    if (e->type == N_SELECT) {
        QInfo *qi = e->info;
        if (qi) for (int i = 0; i < qi->nitems; i++) expr_need(qi->items[i], level, need);
    }
    for (int i = 0; i < e->nkids; i++) expr_need(e->kids[i], level, need);
}
static void collect_needed(IRNode *n, PtrVec *need) {
    if (!n) return;
    switch (n->op) {
    case IR_FILTER: case IR_JOIN: expr_need(n->pred, 0, need); break;
    case IR_PROJECT: for (int i = 0; i < n->nexprs; i++) expr_need(n->exprs[i], 0, need); break;
    case IR_GROUP: case IR_AGGREGATE:
        for (int i = 0; i < n->nkeys; i++) expr_need(n->keys[i], 0, need);
        for (int i = 0; i < n->naggs; i++) expr_need(n->aggs[i], 0, need);
        break;
    default: break;
    }
    if (n->op != IR_VIEW && n->op != IR_DERIVED) {
        collect_needed(n->left, need);
        collect_needed(n->right, need);
    }
}
static void apply_prune(IRNode *n, PtrVec *need) {
    if (!n) return;
    if (n->op == IR_LOAD && !n->no_prune) {
        TableSym *t = ht_get(g_cat.tables, n->table);
        if (t) {
            const char *q = n->alias ? n->alias : n->table;
            char **cols = xcalloc((size_t)t->ncols + 1, sizeof(char *));
            int k = 0;
            for (int i = 0; i < t->ncols; i++) {
                char *key = xsprintf("%s.%s", q, t->cols[i].name);
                for (char *p = key; *p; p++) *p = (char)tolower((unsigned char)*p);
                if (pv_contains_str(need, key)) cols[k++] = xstrdup(t->cols[i].name);
                free(key);
            }
            if (k < t->ncols) {
                n->cols = cols;
                n->ncols = k;
                SB b; sb_init(&b);
                for (int i = 0; i < k; i++) sb_add(&b, "%s%s", i ? ", " : "", cols[i]);
                char *s = sb_take(&b);
                logr("PROJECTION REDUCTION", "LOAD %s reads only %d of %d column(s)%s%s%s", t->name, k, t->ncols,
                     k ? " (" : " (row count only)", s, k ? ")" : "");
                free(s);
            } else free(cols);
        }
    }
    if (n->op != IR_VIEW && n->op != IR_DERIVED) {
        apply_prune(n->left, need);
        apply_prune(n->right, need);
    }
}

/* ======================================================================
 *  5. Redundant operation removal
 * ====================================================================== */
typedef struct { int n; char **q; char **c; } Schema;

static int schema_of(IRNode *n, Schema *s) {
    if (!n) return 0;
    switch (n->op) {
    case IR_LOAD: {
        TableSym *t = ht_get(g_cat.tables, n->table);
        if (!t) return 0;
        const char *q = n->alias ? n->alias : n->table;
        int k = n->cols ? n->ncols : t->ncols;
        s->q = xrealloc(s->q, sizeof(char *) * (size_t)(s->n + k + 1));
        s->c = xrealloc(s->c, sizeof(char *) * (size_t)(s->n + k + 1));
        for (int i = 0; i < k; i++) {
            s->q[s->n] = (char *)q;
            s->c[s->n] = n->cols ? n->cols[i] : t->cols[i].name;
            s->n++;
        }
        return 1;
    }
    case IR_VIEW: case IR_DERIVED:
        s->q = xrealloc(s->q, sizeof(char *) * (size_t)(s->n + n->nout + 1));
        s->c = xrealloc(s->c, sizeof(char *) * (size_t)(s->n + n->nout + 1));
        for (int i = 0; i < n->nout; i++) { s->q[s->n] = n->alias; s->c[s->n] = n->out_names[i]; s->n++; }
        return 1;
    case IR_FILTER: case IR_SORT: case IR_LIMIT: case IR_DISTINCT:
        if (n->post_agg) return 0;
        return schema_of(n->left, s);
    case IR_JOIN:
        return schema_of(n->left, s) && schema_of(n->right, s);
    default:
        return 0;
    }
}

static int identity_project(IRNode *p) {
    if (p->op != IR_PROJECT || p->trim || p->post_agg || p->nexprs != p->nvisible) return 0;
    Schema s = {0};
    if (!schema_of(p->left, &s) || s.n != p->nexprs) { free(s.q); free(s.c); return 0; }
    int ident = 1;
    for (int i = 0; i < p->nexprs && ident; i++) {
        Node *e = p->exprs[i];
        if (e->type != N_COLREF || e->depth != 0 || !e->rqual) ident = 0;
        else if (!ci_eq(e->rqual, s.q[i]) || !ci_eq(e->s2, s.c[i]) || !ci_eq(p->names[i], s.c[i])) ident = 0;
    }
    free(s.q); free(s.c);
    return ident;
}

static IRNode *remove_redundant(IRNode *n) {
    if (!n) return n;
    n->left = remove_redundant(n->left);
    n->right = remove_redundant(n->right);
    if (n->op == IR_PROJECT && identity_project(n)) {
        logr("REDUNDANT PROJECT", "PROJECT keeps every input column unchanged - removed");
        return n->left;
    }
    if (n->op == IR_DISTINCT) {
        IRNode *c = n->left;
        IRNode *base = c->op == IR_PROJECT ? c->left : c;
        while (base && base->op == IR_FILTER) base = base->left;
        if (base && base->op == IR_LOAD) {
            TableSym *t = ht_get(g_cat.tables, base->table);
            KeySym *pk = t ? table_primary_key(t) : NULL;
            const char *q = base->alias ? base->alias : base->table;
            if (pk) {
                int all = 1;
                for (int k = 0; k < pk->ncols && all; k++) {
                    int found = 0;
                    if (c->op == IR_PROJECT) {
                        for (int i = 0; i < c->nvisible; i++) {
                            Node *e = c->exprs[i];
                            if (e->type == N_COLREF && e->rqual && ci_eq(e->rqual, q) && ci_eq(e->s2, pk->cols[k])) found = 1;
                        }
                    } else {
                        Schema s = {0};
                        if (schema_of(c, &s))
                            for (int i = 0; i < s.n; i++) if (ci_eq(s.c[i], pk->cols[k])) found = 1;
                        free(s.q); free(s.c);
                    }
                    if (!found) all = 0;
                }
                if (all) {
                    logr("REDUNDANT DISTINCT", "output contains the PRIMARY KEY of %s, rows are already unique - DISTINCT removed", t->name);
                    return c;
                }
            }
        }
    }
    return n;
}

/* ======================================================================
 *  Driver
 * ====================================================================== */
static IRNode *opt_tree(IRNode *root) {
    if (!root) return root;
    root = drop_true_filters(root);
    root = pushdown(root);
    /* ON-condition pushdown works on every join in the tree */
    PtrVec stack = {0};
    pv_push(&stack, root);
    while (stack.n) {
        IRNode *n = stack.items[--stack.n];
        if (!n) continue;
        if (n->op == IR_JOIN) push_on_conditions(n);
        else { pv_push(&stack, n->left); pv_push(&stack, n->right); }
    }
    free(stack.items);
    stack = (PtrVec){0};
    choose_join_algorithms(root);
    PtrVec need = {0};
    collect_needed(root, &need);
    apply_prune(root, &need);
    root = remove_redundant(root);
    /* recurse into view / derived sub-plans */
    pv_push(&stack, root);
    while (stack.n) {
        IRNode *n = stack.items[--stack.n];
        if (!n) continue;
        if ((n->op == IR_VIEW || n->op == IR_DERIVED) && n->sub) n->sub = opt_tree(n->sub);
        else { pv_push(&stack, n->left); pv_push(&stack, n->right); }
    }
    free(stack.items);
    return root;
}

int optimize_plan(Plan *p) {
    nrewrites = 0;
    IRNode *r = p->root;
    fold_tree(r);
    for (int i = 0; i < p->subplans.n; i++) fold_tree(((SubPlan *)p->subplans.items[i])->root);

    if (r->op == IR_INSERT || r->op == IR_UPDATE || r->op == IR_DELETE) {
        if (r->left) r->left = opt_tree(r->left);
    } else if (r->op != IR_COMMAND) {
        p->root = opt_tree(r);
    }
    for (int i = 0; i < p->subplans.n; i++) {
        SubPlan *sp = p->subplans.items[i];
        sp->root = opt_tree(sp->root);
        sp->query->plan = sp->root;
        if (sp->query->correlated)
            info("SUBQUERY", "SUBQ#%d is correlated - it is re-evaluated for every outer row", sp->id);
        else
            logr("SUBQUERY CACHING", "SUBQ#%d is uncorrelated - evaluated once, result cached and reused", sp->id);
    }
    return nrewrites;
}
