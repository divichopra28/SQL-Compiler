/*
 * ir.c - intermediate code generation (AST -> relational IR) and printing
 */
#include "ir.h"
#include "semantic.h"
#include "symbol_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Plan *cur_plan = NULL;
static int subq_counter = 0;

IRNode *ir_new(IROp op) {
    IRNode *n = xcalloc(1, sizeof(IRNode));
    n->op = op;
    n->limit = -1;
    return n;
}

const char *ir_op_name(IROp op) {
    static const char *names[] = { "LOAD", "VIEW", "DERIVED", "DUAL", "VALUES", "FILTER", "JOIN", "GROUP",
                                   "AGGREGATE", "PROJECT", "DISTINCT", "SORT", "LIMIT", "SETOP", "INSERT",
                                   "UPDATE", "DELETE", "COMMAND" };
    return names[op];
}

static IRNode *gen_query(Node *q);

/* find sub-queries inside an expression and compile them */
static void scan_subqueries(Node *e) {
    if (!e) return;
    if (e->type == N_SUBQUERY || e->type == N_EXISTS || e->type == N_IN_SUBQ) {
        Node *q = e->type == N_IN_SUBQ ? e->kids[1] : e->kids[0];
        if (e->type == N_IN_SUBQ) scan_subqueries(e->kids[0]);
        if (!q->plan) {
            q->plan = gen_query(q);
            SubPlan *sp = xcalloc(1, sizeof(SubPlan));
            sp->holder = e;
            sp->query = q;
            sp->root = q->plan;
            sp->id = ++subq_counter;
            q->subq_id = sp->id;
            if (cur_plan) pv_push(&cur_plan->subplans, sp);
        }
        return;
    }
    if (e->type == N_QUERY) return;
    for (int i = 0; i < e->nkids; i++) scan_subqueries(e->kids[i]);
}

static IRNode *gen_from(Node *t) {
    IRNode *n;
    switch (t->type) {
    case N_TABLE_REF:
        if (t->info) {                       /* a view */
            Node *vq = t->info;
            n = ir_new(IR_VIEW);
            n->table = xstrdup(t->s1);
            n->alias = xstrdup(t->s2 ? t->s2 : t->s1);
            n->ast = vq;
            n->sub = gen_query(vq);
            QInfo *qi = vq->info;
            n->out_names = qi->out_names;
            n->nout = qi->nout;
            return n;
        }
        n = ir_new(IR_LOAD);
        n->table = xstrdup(t->s1);
        n->alias = t->s2 ? xstrdup(t->s2) : NULL;
        return n;
    case N_DERIVED_TABLE: {
        n = ir_new(IR_DERIVED);
        n->alias = xstrdup(t->s2);
        n->ast = t->kids[0];
        n->sub = gen_query(t->kids[0]);
        QInfo *qi = t->kids[0]->info;
        n->out_names = qi->out_names;
        n->nout = qi->nout;
        return n;
    }
    case N_JOIN:
        n = ir_new(IR_JOIN);
        n->join_type = t->op;
        n->left = gen_from(t->kids[0]);
        n->right = gen_from(t->kids[1]);
        n->pred = t->kids[2];
        scan_subqueries(n->pred);
        return n;
    default:
        return ir_new(IR_DUAL);
    }
}

static IRNode *gen_select(Node *sel, QInfo *qq) {
    QInfo *qi = sel->info;
    IRNode *n = sel->kids[1] ? gen_from(sel->kids[1]) : ir_new(IR_DUAL);
    if (sel->kids[2]) {
        IRNode *f = ir_new(IR_FILTER);
        f->left = n;
        f->pred = sel->kids[2];
        f->filter_kind = FK_WHERE;
        scan_subqueries(f->pred);
        n = f;
    }
    int post = 0;
    if (qi->has_agg) {
        if (qi->ngkeys) {
            IRNode *g = ir_new(IR_GROUP);
            g->left = n;
            g->keys = qi->gkeys;
            g->nkeys = qi->ngkeys;
            n = g;
        }
        IRNode *a = ir_new(IR_AGGREGATE);
        a->left = n;
        a->keys = qi->gkeys;
        a->nkeys = qi->ngkeys;
        a->aggs = qi->aggs;
        a->naggs = qi->naggs;
        for (int i = 0; i < qi->naggs; i++) scan_subqueries(qi->aggs[i]);
        n = a;
        post = 1;
    }
    if (sel->kids[4]) {
        IRNode *f = ir_new(IR_FILTER);
        f->left = n;
        f->pred = sel->kids[4];
        f->filter_kind = FK_HAVING;
        f->post_agg = post;
        scan_subqueries(f->pred);
        n = f;
    }
    IRNode *p = ir_new(IR_PROJECT);
    p->left = n;
    p->post_agg = post;
    int hidden = qq ? qq->nhidden : 0;
    p->nexprs = qi->nitems + hidden;
    p->nvisible = qi->nitems;
    p->exprs = xcalloc((size_t)(p->nexprs ? p->nexprs : 1), sizeof(Node *));
    p->names = xcalloc((size_t)(p->nexprs ? p->nexprs : 1), sizeof(char *));
    for (int i = 0; i < qi->nitems; i++) {
        p->exprs[i] = qi->items[i];
        p->names[i] = qi->out_names[i];
        scan_subqueries(qi->items[i]);
    }
    int h = qi->nitems;
    for (int i = 0; qq && i < qq->nsort; i++)
        if (qq->sort_col[i] < 0) {
            p->exprs[h] = qq->sort_expr[i];
            p->names[h] = expr_to_sql(qq->sort_expr[i]);
            scan_subqueries(qq->sort_expr[i]);
            h++;
        }
    n = p;
    if (sel->flag) {
        IRNode *d = ir_new(IR_DISTINCT);
        d->left = n;
        n = d;
    }
    return n;
}

static IRNode *gen_body(Node *b) {
    if (b->type == N_SELECT) return gen_select(b, NULL);
    IRNode *s = ir_new(IR_SETOP);
    s->setop = b->op;
    s->set_all = b->flag;
    s->left = gen_body(b->kids[0]);
    s->right = gen_body(b->kids[1]);
    return s;
}

static IRNode *gen_query(Node *q) {
    QInfo *qi = q->info;
    Node *body = q->kids[0];
    IRNode *n = body->type == N_SELECT ? gen_select(body, qi) : gen_body(body);
    if (qi->nsort) {
        IRNode *s = ir_new(IR_SORT);
        s->left = n;
        s->nsort = qi->nsort;
        s->sort_cols = xcalloc((size_t)qi->nsort, sizeof(int));
        s->sort_desc = xcalloc((size_t)qi->nsort, sizeof(int));
        s->sort_names = xcalloc((size_t)qi->nsort, sizeof(char *));
        int h = qi->nout;
        for (int i = 0; i < qi->nsort; i++) {
            if (qi->sort_col[i] >= 0) {
                s->sort_cols[i] = qi->sort_col[i];
                s->sort_names[i] = qi->out_names[qi->sort_col[i]];
            } else {
                s->sort_cols[i] = h++;
                s->sort_names[i] = expr_to_sql(qi->sort_expr[i]);
            }
            s->sort_desc[i] = qi->sort_desc[i];
        }
        n = s;
    }
    if (node_kid(q, 2)) {
        IRNode *l = ir_new(IR_LIMIT);
        l->left = n;
        l->limit = q->kids[2]->ival;
        l->offset = node_kid(q, 3) ? q->kids[3]->ival : 0;
        n = l;
    }
    if (qi->nhidden) {
        IRNode *p = ir_new(IR_PROJECT);
        p->left = n;
        p->trim = 1;
        p->nvisible = qi->nout;
        p->names = qi->out_names;
        n = p;
    }
    return n;
}

Plan *ir_generate(Node *stmt) {
    Plan *p = xcalloc(1, sizeof(Plan));
    p->stmt = stmt;
    cur_plan = p;
    subq_counter = 0;
    if (stmt->type == N_EXPLAIN) { p->explain = 1; stmt = stmt->kids[0]; }
    IRNode *n;
    switch (stmt->type) {
    case N_QUERY:
        p->root = gen_query(stmt);
        break;
    case N_INSERT: {
        n = ir_new(IR_INSERT);
        n->table = xstrdup(stmt->s1);
        n->ast = stmt;
        Node *src = stmt->kids[1];
        if (src->type == N_QUERY) n->left = gen_query(src);
        else {
            IRNode *v = ir_new(IR_VALUES);
            v->ast = src;
            for (int r = 0; r < src->nkids; r++)
                for (int i = 0; i < src->kids[r]->nkids; i++) scan_subqueries(src->kids[r]->kids[i]);
            n->left = v;
        }
        p->root = n;
        break;
    }
    case N_UPDATE: case N_DELETE: {
        IRNode *l = ir_new(IR_LOAD);
        l->table = xstrdup(stmt->s1);
        l->no_prune = 1;
        Node *where = stmt->type == N_UPDATE ? stmt->kids[1] : stmt->kids[0];
        IRNode *src = l;
        if (where) {
            IRNode *f = ir_new(IR_FILTER);
            f->left = l;
            f->pred = where;
            f->filter_kind = FK_WHERE;
            scan_subqueries(where);
            src = f;
        }
        n = ir_new(stmt->type == N_UPDATE ? IR_UPDATE : IR_DELETE);
        n->table = xstrdup(stmt->s1);
        n->ast = stmt;
        n->left = src;
        if (stmt->type == N_UPDATE)
            for (int i = 0; i < stmt->kids[0]->nkids; i++) scan_subqueries(stmt->kids[0]->kids[i]->kids[0]);
        p->root = n;
        break;
    }
    default:
        n = ir_new(IR_COMMAND);
        n->ast = stmt;
        p->root = n;
    }
    cur_plan = NULL;
    return p;
}

int ir_count(IRNode *n) {
    if (!n) return 0;
    return 1 + ir_count(n->left) + ir_count(n->right) + ir_count(n->sub);
}

/* ======================================================================
 *  Printing
 * ====================================================================== */
static char *list_exprs(Node **v, int n) {
    SB b; sb_init(&b);
    for (int i = 0; i < n; i++) {
        char *s = expr_to_sql(v[i]);
        sb_add(&b, "%s%s", i ? ", " : "", s);
        free(s);
    }
    return sb_take(&b);
}

static char *command_text(Node *s) {
    SB b; sb_init(&b);
    switch (s->type) {
    case N_CREATE_TABLE: {
        TableSym *t = s->info;
        if (!t) { sb_add(&b, "NOOP  (table %s already exists)", s->s1); break; }
        sb_add(&b, "CREATE_TABLE %s {", t->name);
        for (int i = 0; i < t->ncols; i++) {
            char *ts = column_type_str(&t->cols[i]);
            sb_add(&b, "%s%s:%s", i ? ", " : "", t->cols[i].name, ts);
            free(ts);
        }
        sb_add(&b, "}  keys=%d fks=%d checks=%d", t->nkeys, t->nfks, t->nchecks);
        break;
    }
    case N_CREATE_VIEW: sb_add(&b, "CREATE_VIEW %s AS <query>", s->s1); break;
    case N_CREATE_INDEX: sb_add(&b, "CREATE_%sINDEX %s ON %s", s->flag ? "UNIQUE_" : "", s->s1, s->s2); break;
    case N_ALTER_TABLE: {
        Node *a = s->kids[0];
        const char *w[] = { "", "ADD_COLUMN", "DROP_COLUMN", "MODIFY_COLUMN", "RENAME_COLUMN", "RENAME_TABLE" };
        sb_add(&b, "ALTER_TABLE %s %s ", s->s1, w[a->op]);
        if (a->op == A_ADD || a->op == A_MODIFY) sb_add(&b, "%s", a->kids[0]->s1);
        else if (a->op == A_DROP) sb_add(&b, "%s", a->s1);
        else if (a->op == A_RENAME_COL) sb_add(&b, "%s -> %s", a->s1, a->s2);
        else sb_add(&b, "-> %s", a->s2);
        break;
    }
    case N_DROP_TABLE: sb_add(&b, "DROP_TABLE %s", s->s1); break;
    case N_DROP_VIEW: sb_add(&b, "DROP_VIEW %s", s->s1); break;
    case N_DROP_INDEX: sb_add(&b, "DROP_INDEX %s", s->s1); break;
    case N_TRUNCATE: sb_add(&b, "TRUNCATE %s", s->s1); break;
    case N_CREATE_DATABASE: sb_add(&b, "CREATE_DATABASE %s", s->s1); break;
    case N_DROP_DATABASE: sb_add(&b, "DROP_DATABASE %s", s->s1); break;
    case N_USE: sb_add(&b, "USE_DATABASE %s", s->s1); break;
    case N_GRANT: case N_REVOKE: {
        int m = 0;
        for (int i = 0; i < s->kids[0]->nkids; i++) m |= s->kids[0]->kids[i]->op;
        char *ps = priv_mask_str(m);
        sb_add(&b, "%s {%s} ON %s %s ", s->type == N_GRANT ? "GRANT" : "REVOKE", ps, s->s1,
               s->type == N_GRANT ? "TO" : "FROM");
        free(ps);
        for (int i = 0; i < s->kids[1]->nkids; i++) sb_add(&b, "%s%s", i ? ", " : "", s->kids[1]->kids[i]->s1);
        break;
    }
    case N_CREATE_USER: sb_add(&b, "CREATE_USER %s", s->s1); break;
    case N_DROP_USER: sb_add(&b, "DROP_USER %s", s->s1); break;
    case N_CONNECT: sb_add(&b, "SET_CURRENT_USER %s", s->s1); break;
    case N_BEGIN: sb_add(&b, "TXN_BEGIN"); break;
    case N_COMMIT: sb_add(&b, "TXN_COMMIT"); break;
    case N_ROLLBACK: sb_add(&b, "TXN_ROLLBACK"); break;
    case N_SAVEPOINT: sb_add(&b, "TXN_SAVEPOINT %s", s->s1); break;
    case N_ROLLBACK_TO: sb_add(&b, "TXN_ROLLBACK_TO %s", s->s1); break;
    case N_RELEASE: sb_add(&b, "TXN_RELEASE %s", s->s1); break;
    case N_SHOW: sb_add(&b, "SHOW_CATALOG"); break;
    case N_DESCRIBE: sb_add(&b, "DESCRIBE %s", s->s1); break;
    default: sb_add(&b, "%s", stmt_name(s));
    }
    return sb_take(&b);
}

static int print_node(IRNode *n, int *counter, const char *indent) {
    if (!n) return 0;
    int sub = 0, l = 0, r = 0;
    if (n->sub) sub = print_node(n->sub, counter, indent);
    if (n->left) l = print_node(n->left, counter, indent);
    if (n->right) r = print_node(n->right, counter, indent);
    n->id = ++(*counter);
    char *s = NULL;
    SB b; sb_init(&b);
    sb_add(&b, "T%d = ", n->id);
    switch (n->op) {
    case IR_LOAD:
        sb_add(&b, "LOAD %s", n->table);
        if (n->alias) sb_add(&b, " AS %s", n->alias);
        if (n->cols) {
            sb_add(&b, " (");
            for (int i = 0; i < n->ncols; i++) sb_add(&b, "%s%s", i ? ", " : "", n->cols[i]);
            sb_add(&b, ")");
        }
        break;
    case IR_VIEW: sb_add(&b, "VIEW %s (T%d) AS %s", n->table, sub, n->alias); break;
    case IR_DERIVED: sb_add(&b, "DERIVED (T%d) AS %s", sub, n->alias); break;
    case IR_DUAL: sb_add(&b, "DUAL  (one empty row)"); break;
    case IR_VALUES: {
        Node *rows = n->ast;
        sb_add(&b, "VALUES ");
        for (int r2 = 0; r2 < rows->nkids; r2++) {
            char *x = list_exprs(rows->kids[r2]->kids, rows->kids[r2]->nkids);
            sb_add(&b, "%s(%s)", r2 ? ", " : "", x);
            free(x);
        }
        break;
    }
    case IR_FILTER:
        s = expr_to_sql(n->pred);
        sb_add(&b, "FILTER T%d %s %s", l, n->filter_kind == FK_HAVING ? "HAVING" : "WHERE", s);
        break;
    case IR_JOIN: {
        const char *jn = n->join_type == J_INNER ? "JOIN" : n->join_type == J_LEFT ? "LEFT JOIN" :
                         n->join_type == J_RIGHT ? "RIGHT JOIN" : n->join_type == J_FULL ? "FULL JOIN" : "CROSS JOIN";
        sb_add(&b, "%s T%d AND T%d", jn, l, r);
        if (n->pred && n->join_type != J_CROSS) { s = expr_to_sql(n->pred); sb_add(&b, " ON %s", s); }
        break;
    }
    case IR_GROUP:
        s = list_exprs(n->keys, n->nkeys);
        sb_add(&b, "GROUP T%d BY %s", l, s);
        break;
    case IR_AGGREGATE:
        s = list_exprs(n->aggs, n->naggs);
        sb_add(&b, "AGGREGATE T%d %s", l, n->naggs ? s : "(group keys only)");
        if (!n->nkeys) sb_add(&b, "  [whole input as one group]");
        break;
    case IR_PROJECT:
        if (n->trim) {
            sb_add(&b, "PROJECT T%d (", l);
            for (int i = 0; i < n->nvisible; i++) sb_add(&b, "%s%s", i ? ", " : "", n->names[i]);
            sb_add(&b, ")  [drop sort keys]");
            break;
        }
        sb_add(&b, "PROJECT T%d (", l);
        for (int i = 0; i < n->nexprs; i++) {
            char *x = expr_to_sql(n->exprs[i]);
            if (i == n->nvisible) sb_add(&b, " | hidden: ");
            else if (i) sb_add(&b, ", ");
            sb_add(&b, "%s", x);
            Node *e = n->exprs[i];
            int is_plain = e->type == N_COLREF && ci_eq(e->s2, n->names[i]);
            if (i < n->nvisible && !is_plain && strcmp(x, n->names[i]) != 0) sb_add(&b, " AS %s", n->names[i]);
            free(x);
        }
        sb_add(&b, ")");
        break;
    case IR_DISTINCT: sb_add(&b, "DISTINCT T%d", l); break;
    case IR_SORT:
        sb_add(&b, "SORT T%d BY ", l);
        for (int i = 0; i < n->nsort; i++)
            sb_add(&b, "%s%s %s", i ? ", " : "", n->sort_names[i], n->sort_desc[i] ? "DESC" : "ASC");
        break;
    case IR_LIMIT:
        sb_add(&b, "LIMIT T%d %lld", l, n->limit);
        if (n->offset) sb_add(&b, " OFFSET %lld", n->offset);
        break;
    case IR_SETOP: sb_add(&b, "%s T%d AND T%d", setop_name(n->setop, n->set_all), l, r); break;
    case IR_INSERT: {
        sb_add(&b, "INSERT INTO %s", n->table);
        Node *cl = n->ast->kids[0];
        if (cl) {
            sb_add(&b, " (");
            for (int i = 0; i < cl->nkids; i++) sb_add(&b, "%s%s", i ? ", " : "", cl->kids[i]->s1);
            sb_add(&b, ")");
        }
        sb_add(&b, " FROM T%d", l);
        break;
    }
    case IR_UPDATE: {
        sb_add(&b, "UPDATE %s SET ", n->table);
        Node *al = n->ast->kids[0];
        for (int i = 0; i < al->nkids; i++) {
            char *x = expr_to_sql(al->kids[i]->kids[0]);
            sb_add(&b, "%s%s = %s", i ? ", " : "", al->kids[i]->s1, x);
            free(x);
        }
        sb_add(&b, " ROWS T%d", l);
        break;
    }
    case IR_DELETE: sb_add(&b, "DELETE FROM %s ROWS T%d", n->table, l); break;
    case IR_COMMAND: s = command_text(n->ast); sb_add(&b, "%s", s); break;
    }
    free(s);
    char *line = sb_take(&b);
    printf("%s%s", indent, line);
    if (n->note) printf("    %s; %s%s", COL_DIM, n->note, COL_RESET);
    printf("\n");
    free(line);
    return n->id;
}

void ir_print(Plan *p) {
    int counter = 0;
    g_unparse_subq_ref = 1;
    print_node(p->root, &counter, "");
    for (int i = 0; i < p->subplans.n; i++) {
        SubPlan *sp = p->subplans.items[i];
        printf("\n%sSUBQ#%d%s  %s\n", COL_BOLD, sp->id, COL_RESET,
               sp->query->correlated ? "(correlated: evaluated for each outer row)" : "(uncorrelated)");
        print_node(sp->root, &counter, "  ");
    }
    g_unparse_subq_ref = 0;
}
