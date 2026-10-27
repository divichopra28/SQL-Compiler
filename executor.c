/*
 * executor.c - IR interpreter and storage layer
 */
#include "executor.h"
#include "symbol_table.h"
#include "semantic.h"
#include "transaction.h"
#include "diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <glob.h>

sqlite3 *g_db = NULL;
char     g_dbname[MAX_NAME] = "sqlcompiler";
char     g_dbdir[512] = "database";

/* ======================================================================
 *  Storage helpers
 * ====================================================================== */
static char *qid(const char *name) {            /* "quoted" identifier */
    SB b; sb_init(&b);
    sb_addc(&b, '"');
    for (const char *p = name; *p; p++) { if (*p == '"') sb_addc(&b, '"'); sb_addc(&b, *p); }
    sb_addc(&b, '"');
    return sb_take(&b);
}

static void storage_exec(const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(g_db, sql, NULL, NULL, &err) != SQLITE_OK) {
        char msg[512];
        snprintf(msg, sizeof msg, "%s", err ? err : "unknown error");
        sqlite3_free(err);
        exec_error(NULL, "storage engine error: %s", msg);
    }
}

static void bind_value(sqlite3_stmt *st, int i, Value v) {
    switch (v.k) {
    case V_NULL: sqlite3_bind_null(st, i); break;
    case V_INT: case V_BOOL: sqlite3_bind_int64(st, i, v.i); break;
    case V_FLOAT: sqlite3_bind_double(st, i, v.f); break;
    case V_STR: sqlite3_bind_text(st, i, v.s, -1, SQLITE_TRANSIENT); break;
    }
}

static sqlite3_stmt *prepare(const char *sql) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK)
        exec_error(NULL, "storage engine error: %s", sqlite3_errmsg(g_db));
    return st;
}

static void step_done(sqlite3_stmt *st) {
    int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        char msg[512];
        snprintf(msg, sizeof msg, "%s", sqlite3_errmsg(g_db));
        sqlite3_finalize(st);
        exec_error(NULL, "storage engine error: %s", msg);
    }
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
}

/* read rows of a base table: SELECT rowid, cols... FROM table */
static Relation *load_table(TableSym *t, const char *qual, char **cols, int ncols) {
    int all = cols == NULL;
    int k = all ? t->ncols : ncols;
    Relation *r = rel_new(k);
    int *ci = xcalloc((size_t)(k ? k : 1), sizeof(int));
    SB b; sb_init(&b);
    sb_add(&b, "SELECT rowid");
    for (int i = 0; i < k; i++) {
        ci[i] = all ? i : column_index(t, cols[i]);
        ColumnSym *c = &t->cols[ci[i]];
        char *q = qid(c->name);
        sb_add(&b, ", %s", q);
        free(q);
        rel_set_col(r, i, qual, c->name, c->type, c->len);
    }
    char *tq = qid(t->name);
    sb_add(&b, " FROM %s ORDER BY rowid", tq);
    free(tq);
    char *sql = sb_take(&b);
    sqlite3_stmt *st = prepare(sql);
    free(sql);
    r->has_rowids = 1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        Value *row = row_new(k);
        for (int i = 0; i < k; i++) {
            int dt = t->cols[ci[i]].type;
            int ct = sqlite3_column_type(st, i + 1);
            switch (ct) {
            case SQLITE_NULL: row[i] = v_null(); break;
            case SQLITE_INTEGER: {
                long long x = sqlite3_column_int64(st, i + 1);
                if (dt == DT_BOOLEAN) row[i] = v_bool((int)x);
                else if (dt_is_numeric(dt) && !dt_is_integer(dt)) row[i] = v_float((double)x);
                else if (dt_is_string(dt) || dt_is_temporal(dt)) row[i] = v_str((const char *)sqlite3_column_text(st, i + 1));
                else row[i] = v_int(x);
                break;
            }
            case SQLITE_FLOAT: {
                double x = sqlite3_column_double(st, i + 1);
                if (dt_is_integer(dt)) row[i] = v_int((long long)llround(x));
                else if (dt_is_string(dt)) row[i] = v_str((const char *)sqlite3_column_text(st, i + 1));
                else row[i] = v_float(x);
                break;
            }
            default:
                row[i] = v_str((const char *)sqlite3_column_text(st, i + 1));
            }
        }
        rel_add_row(r, row, sqlite3_column_int64(st, 0));
    }
    sqlite3_finalize(st);
    free(ci);
    return r;
}

static TableSym *table_or_die(const char *name) {
    TableSym *t = ht_get(g_cat.tables, name);
    if (!t) exec_error(NULL, "table '%s' no longer exists", name);
    return t;
}

/* ======================================================================
 *  Row keys and a small string hash set (DISTINCT, set ops, hash join)
 * ====================================================================== */
static void key_value(SB *b, Value v) {
    switch (v.k) {
    case V_NULL: sb_add(b, "\x01N"); break;
    case V_BOOL: sb_add(b, "\x01" "b%lld", v.i); break;
    case V_INT: sb_add(b, "\x01#%lld", v.i); break;
    case V_FLOAT:
        if (v.f == floor(v.f) && fabs(v.f) < 9e15) sb_add(b, "\x01#%lld", (long long)v.f);
        else sb_add(b, "\x01#%.17g", v.f);
        break;
    case V_STR: sb_add(b, "\x01s%s", v.s); break;
    }
}
static char *row_key(Value *row, int n) {
    SB b; sb_init(&b);
    for (int i = 0; i < n; i++) key_value(&b, row[i]);
    return sb_take(&b);
}

typedef struct SEnt { char *k; int count; int *idx; int nidx, cidx; struct SEnt *next; } SEnt;
typedef struct { SEnt **b; int nb; } StrSet;
static unsigned hstr(const char *s) { unsigned h = 2166136261u; for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u; return h; }
static StrSet *ss_new(int n) { StrSet *s = xcalloc(1, sizeof(StrSet)); s->nb = n < 64 ? 64 : n * 2; s->b = xcalloc((size_t)s->nb, sizeof(SEnt *)); return s; }
static SEnt *ss_get(StrSet *s, const char *k) {
    for (SEnt *e = s->b[hstr(k) % (unsigned)s->nb]; e; e = e->next) if (!strcmp(e->k, k)) return e;
    return NULL;
}
static SEnt *ss_add(StrSet *s, const char *k) {
    SEnt *e = ss_get(s, k);
    if (e) { e->count++; return e; }
    unsigned h = hstr(k) % (unsigned)s->nb;
    e = xcalloc(1, sizeof(SEnt));
    e->k = xstrdup(k);
    e->count = 1;
    e->next = s->b[h];
    s->b[h] = e;
    return e;
}
static void ss_push_idx(SEnt *e, int i) {
    if (e->nidx == e->cidx) { e->cidx = e->cidx ? e->cidx * 2 : 4; e->idx = xrealloc(e->idx, sizeof(int) * (size_t)e->cidx); }
    e->idx[e->nidx++] = i;
}
static void ss_free(StrSet *s) {
    for (int i = 0; i < s->nb; i++) {
        SEnt *e = s->b[i];
        while (e) { SEnt *n = e->next; free(e->k); free(e->idx); free(e); e = n; }
    }
    free(s->b);
    free(s);
}

/* ======================================================================
 *  Relation helpers
 * ====================================================================== */
static Relation *rel_like(Relation *in) {
    Relation *r = rel_new(in->ncols);
    for (int i = 0; i < in->ncols; i++)
        rel_set_col(r, i, in->cols[i].qual, in->cols[i].name, in->cols[i].dtype, in->cols[i].dlen);
    r->sid = in->sid;
    r->has_rowids = in->has_rowids;
    return r;
}
static void free_row(Value *row, int n) {
    for (int i = 0; i < n; i++) v_free(&row[i]);
    free(row);
}
static Value *copy_row(Value *row, int n) {
    Value *r = row_new(n);
    for (int i = 0; i < n; i++) r[i] = v_copy(row[i]);
    return r;
}
/* release the relation shell after its rows were moved elsewhere */
static void rel_free_shell(Relation *r) { r->nrows = 0; rel_free(r); }

/* ======================================================================
 *  Operators
 * ====================================================================== */
static Relation *op_filter(IRNode *n, Env *outer) {
    Relation *in = exec_node(n->left, outer);
    Relation *out = rel_like(in);
    for (int i = 0; i < in->nrows; i++) {
        Env env = { in, in->rows[i], n->post_agg, outer };
        if (eval_predicate(n->pred, &env)) rel_add_row(out, in->rows[i], in->rowids[i]);
        else free_row(in->rows[i], in->ncols);
    }
    rel_free_shell(in);
    return out;
}

static Relation *op_join(IRNode *n, Env *outer) {
    Relation *L = exec_node(n->left, outer);
    Relation *R = exec_node(n->right, outer);
    int nc = L->ncols + R->ncols;
    Relation *out = rel_new(nc);
    for (int i = 0; i < L->ncols; i++) rel_set_col(out, i, L->cols[i].qual, L->cols[i].name, L->cols[i].dtype, L->cols[i].dlen);
    for (int i = 0; i < R->ncols; i++) rel_set_col(out, L->ncols + i, R->cols[i].qual, R->cols[i].name, R->cols[i].dtype, R->cols[i].dlen);
    Value *buf = row_new(nc);
    Env env = { out, buf, 0, outer };
    int *rmatched = xcalloc((size_t)(R->nrows ? R->nrows : 1), sizeof(int));
    int jt = n->join_type;
    Node *pred = jt == J_CROSS ? NULL : n->pred;

    StrSet *hash = NULL;
    if (n->hkey_l && R->nrows) {                /* build phase */
        hash = ss_new(R->nrows);
        for (int j = 0; j < R->nrows; j++) {
            Env re = { R, R->rows[j], 0, outer };
            Value k = eval_expr(n->hkey_r, &re);
            if (k.k != V_NULL) {
                SB b; sb_init(&b);
                key_value(&b, k);
                char *ks = sb_take(&b);
                ss_push_idx(ss_add(hash, ks), j);
                free(ks);
            }
            v_free(&k);
        }
    }
    for (int i = 0; i < L->nrows; i++) {
        int matched = 0;
        int *cand = NULL, ncand = R->nrows;
        if (hash) {                             /* probe phase */
            Env le = { L, L->rows[i], 0, outer };
            Value k = eval_expr(n->hkey_l, &le);
            SEnt *e = NULL;
            if (k.k != V_NULL) {
                SB b; sb_init(&b);
                key_value(&b, k);
                char *ks = sb_take(&b);
                e = ss_get(hash, ks);
                free(ks);
            }
            v_free(&k);
            cand = e ? e->idx : NULL;
            ncand = e ? e->nidx : 0;
        } else if (n->hkey_l) ncand = 0;
        memcpy(buf, L->rows[i], sizeof(Value) * (size_t)L->ncols);
        for (int c = 0; c < ncand; c++) {
            int j = cand ? cand[c] : c;
            memcpy(buf + L->ncols, R->rows[j], sizeof(Value) * (size_t)R->ncols);
            if (!pred || eval_predicate(pred, &env)) {
                rel_add_row(out, copy_row(buf, nc), 0);
                matched = 1;
                rmatched[j] = 1;
            }
        }
        if (!matched && (jt == J_LEFT || jt == J_FULL)) {
            Value *row = row_new(nc);
            for (int k = 0; k < L->ncols; k++) row[k] = v_copy(L->rows[i][k]);
            rel_add_row(out, row, 0);
        }
    }
    if (jt == J_RIGHT || jt == J_FULL) {
        for (int j = 0; j < R->nrows; j++) {
            if (rmatched[j]) continue;
            Value *row = row_new(nc);
            for (int k = 0; k < R->ncols; k++) row[L->ncols + k] = v_copy(R->rows[j][k]);
            rel_add_row(out, row, 0);
        }
    }
    if (hash) ss_free(hash);
    free(buf);
    free(rmatched);
    rel_free(L);
    rel_free(R);
    return out;
}

/* ---- GROUP: sort rows by the grouping keys and mark group boundaries ---- */
static Value **g_sort_keys;
static int g_sort_nkeys;
static int *g_sort_desc;
static int cmp_nulls_first(Value a, Value b) {
    if (a.k == V_NULL || b.k == V_NULL) return (a.k != V_NULL) - (b.k != V_NULL);
    return v_compare(a, b);
}
static int cmp_idx(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    for (int k = 0; k < g_sort_nkeys; k++) {
        int c = cmp_nulls_first(g_sort_keys[a][k], g_sort_keys[b][k]);
        if (g_sort_desc && g_sort_desc[k]) c = -c;
        if (c) return c;
    }
    return (a > b) - (a < b);                 /* stable */
}

static Relation *op_group(IRNode *n, Env *outer) {
    Relation *in = exec_node(n->left, outer);
    int N = in->nrows, K = n->nkeys;
    Value **keys = xcalloc((size_t)(N ? N : 1), sizeof(Value *));
    for (int i = 0; i < N; i++) {
        keys[i] = row_new(K);
        Env env = { in, in->rows[i], 0, outer };
        for (int k = 0; k < K; k++) keys[i][k] = eval_expr(n->keys[k], &env);
    }
    int *idx = xcalloc((size_t)(N ? N : 1), sizeof(int));
    for (int i = 0; i < N; i++) idx[i] = i;
    g_sort_keys = keys; g_sort_nkeys = K; g_sort_desc = NULL;
    qsort(idx, (size_t)N, sizeof(int), cmp_idx);
    Relation *out = rel_like(in);
    out->group_start = xcalloc((size_t)(N + 1), sizeof(int));
    for (int i = 0; i < N; i++) {
        int r = idx[i];
        if (i == 0) out->group_start[out->ngroups++] = 0;
        else {
            int prev = idx[i - 1], same = 1;
            for (int k = 0; k < K && same; k++) if (!v_same(keys[prev][k], keys[r][k])) same = 0;
            if (!same) out->group_start[out->ngroups++] = i;
        }
        rel_add_row(out, in->rows[r], in->rowids[r]);
    }
    for (int i = 0; i < N; i++) free_row(keys[i], K);
    free(keys);
    free(idx);
    rel_free_shell(in);
    return out;
}

static Value compute_agg(Node *a, Relation *in, int s, int e, Env *outer) {
    if (a->op == AGG_COUNT_STAR) return v_int(e - s);
    PtrVec vals = {0};
    Value *store = xcalloc((size_t)(e - s + 1), sizeof(Value));
    int nv = 0;
    for (int i = s; i < e; i++) {
        Env env = { in, in->rows[i], 0, outer };
        Value v = eval_expr(a->kids[0], &env);
        if (v.k == V_NULL) continue;
        if (a->flag) {                           /* DISTINCT */
            int dup = 0;
            for (int j = 0; j < nv && !dup; j++) if (v_compare(store[j], v) == 0) dup = 1;
            if (dup) { v_free(&v); continue; }
        }
        store[nv++] = v;
    }
    (void)vals;
    Value r = v_null();
    switch (a->op) {
    case AGG_COUNT: r = v_int(nv); break;
    case AGG_SUM: case AGG_AVG: {
        if (!nv) break;
        int all_int = 1;
        long long isum = 0;
        double fsum = 0;
        for (int j = 0; j < nv; j++) {
            if (store[j].k != V_INT) all_int = 0;
            isum += store[j].i;
            fsum += v_num(store[j]);
        }
        if (a->op == AGG_AVG) r = v_float(fsum / nv);
        else r = all_int ? v_int(isum) : v_float(fsum);
        break;
    }
    case AGG_MIN: case AGG_MAX:
        for (int j = 0; j < nv; j++) {
            if (r.k == V_NULL) { r = v_copy(store[j]); continue; }
            int c = v_compare(store[j], r);
            if ((a->op == AGG_MIN && c < 0) || (a->op == AGG_MAX && c > 0)) { v_free(&r); r = v_copy(store[j]); }
        }
        break;
    }
    for (int j = 0; j < nv; j++) v_free(&store[j]);
    free(store);
    return r;
}

static Relation *op_aggregate(IRNode *n, Env *outer) {
    Relation *in = exec_node(n->left, outer);
    int K = n->nkeys, A = n->naggs;
    Relation *out = rel_new(K + A);
    for (int k = 0; k < K; k++) {
        Node *e = n->keys[k];
        char *nm = e->type == N_COLREF ? xstrdup(e->s2) : expr_to_sql(e);
        rel_set_col(out, k, e->type == N_COLREF ? e->rqual : NULL, nm, e->dtype, e->dlen);
        free(nm);
    }
    for (int a = 0; a < A; a++) {
        char *nm = expr_to_sql(n->aggs[a]);
        rel_set_col(out, K + a, NULL, nm, n->aggs[a]->dtype, 0);
        free(nm);
    }
    int ngroups;
    int *starts;
    int single[1] = { 0 };
    if (K > 0) { ngroups = in->ngroups; starts = in->group_start; }
    else { ngroups = 1; starts = single; }
    for (int g = 0; g < ngroups; g++) {
        int s = starts[g];
        int e = (g + 1 < ngroups) ? starts[g + 1] : in->nrows;
        Value *row = row_new(K + A);
        if (e > s) {
            Env env = { in, in->rows[s], 0, outer };
            for (int k = 0; k < K; k++) row[k] = eval_expr(n->keys[k], &env);
        }
        for (int a = 0; a < A; a++) row[K + a] = compute_agg(n->aggs[a], in, s, e, outer);
        rel_add_row(out, row, 0);
    }
    rel_free(in);
    return out;
}

static Relation *op_project(IRNode *n, Env *outer) {
    Relation *in = exec_node(n->left, outer);
    if (n->trim) {
        Relation *out = rel_new(n->nvisible);
        for (int i = 0; i < n->nvisible; i++)
            rel_set_col(out, i, in->cols[i].qual, in->cols[i].name, in->cols[i].dtype, in->cols[i].dlen);
        for (int r = 0; r < in->nrows; r++) {
            Value *row = row_new(n->nvisible);
            for (int i = 0; i < in->ncols; i++) {
                if (i < n->nvisible) row[i] = in->rows[r][i];
                else v_free(&in->rows[r][i]);
            }
            free(in->rows[r]);
            rel_add_row(out, row, 0);
        }
        rel_free_shell(in);
        return out;
    }
    Relation *out = rel_new(n->nexprs);
    for (int i = 0; i < n->nexprs; i++) {
        Node *e = n->exprs[i];
        rel_set_col(out, i, e->type == N_COLREF ? e->rqual : NULL, n->names[i], e->dtype, e->dlen);
    }
    for (int r = 0; r < in->nrows; r++) {
        Env env = { in, in->rows[r], n->post_agg, outer };
        Value *row = row_new(n->nexprs);
        for (int i = 0; i < n->nexprs; i++) row[i] = eval_expr(n->exprs[i], &env);
        rel_add_row(out, row, 0);
    }
    rel_free(in);
    return out;
}

static Relation *op_distinct(Relation *in) {
    Relation *out = rel_like(in);
    out->has_rowids = 0;
    StrSet *seen = ss_new(in->nrows);
    for (int i = 0; i < in->nrows; i++) {
        char *k = row_key(in->rows[i], in->ncols);
        if (ss_get(seen, k)) free_row(in->rows[i], in->ncols);
        else { ss_add(seen, k); rel_add_row(out, in->rows[i], 0); }
        free(k);
    }
    ss_free(seen);
    rel_free_shell(in);
    return out;
}

static Relation *op_sort(IRNode *n, Env *outer) {
    Relation *in = exec_node(n->left, outer);
    int N = in->nrows;
    Value **keys = xcalloc((size_t)(N ? N : 1), sizeof(Value *));
    for (int i = 0; i < N; i++) {
        keys[i] = row_new(n->nsort);
        for (int k = 0; k < n->nsort; k++) keys[i][k] = in->rows[i][n->sort_cols[k]];   /* shallow */
    }
    int *idx = xcalloc((size_t)(N ? N : 1), sizeof(int));
    for (int i = 0; i < N; i++) idx[i] = i;
    g_sort_keys = keys; g_sort_nkeys = n->nsort; g_sort_desc = n->sort_desc;
    qsort(idx, (size_t)N, sizeof(int), cmp_idx);
    Relation *out = rel_like(in);
    for (int i = 0; i < N; i++) rel_add_row(out, in->rows[idx[i]], in->rowids[idx[i]]);
    for (int i = 0; i < N; i++) free(keys[i]);
    free(keys);
    free(idx);
    rel_free_shell(in);
    return out;
}

static Relation *op_limit(IRNode *n, Env *outer) {
    Relation *in = exec_node(n->left, outer);
    Relation *out = rel_like(in);
    for (int i = 0; i < in->nrows; i++) {
        long long pos = i - n->offset;
        if (i >= n->offset && (n->limit < 0 || pos < n->limit)) rel_add_row(out, in->rows[i], in->rowids[i]);
        else free_row(in->rows[i], in->ncols);
    }
    rel_free_shell(in);
    return out;
}

static Relation *op_setop(IRNode *n, Env *outer) {
    Relation *L = exec_node(n->left, outer);
    Relation *R = exec_node(n->right, outer);
    Relation *out = rel_new(L->ncols);
    for (int i = 0; i < L->ncols; i++) {
        int dt = L->cols[i].dtype;
        if (dt == DT_NULL || (dt_is_integer(dt) && dt_is_numeric(R->cols[i].dtype) && !dt_is_integer(R->cols[i].dtype)))
            dt = R->cols[i].dtype;
        rel_set_col(out, i, NULL, L->cols[i].name, dt, L->cols[i].dlen);
    }
    if (n->setop == S_UNION) {
        for (int i = 0; i < L->nrows; i++) rel_add_row(out, L->rows[i], 0);
        for (int i = 0; i < R->nrows; i++) rel_add_row(out, R->rows[i], 0);
        rel_free_shell(L);
        rel_free_shell(R);
        return n->set_all ? out : op_distinct(out);
    }
    StrSet *rs = ss_new(R->nrows), *emitted = ss_new(L->nrows);
    for (int i = 0; i < R->nrows; i++) { char *k = row_key(R->rows[i], R->ncols); ss_add(rs, k); free(k); }
    for (int i = 0; i < L->nrows; i++) {
        char *k = row_key(L->rows[i], L->ncols);
        SEnt *e = ss_get(rs, k);
        int keep;
        if (n->setop == S_INTERSECT) {
            if (n->set_all) { keep = e && e->count > 0; if (keep) e->count--; }
            else keep = e && !ss_get(emitted, k);
        } else {
            if (n->set_all) { keep = !(e && e->count > 0); if (!keep) e->count--; }
            else keep = !e && !ss_get(emitted, k);
        }
        if (keep) { ss_add(emitted, k); rel_add_row(out, L->rows[i], 0); }
        else free_row(L->rows[i], L->ncols);
        free(k);
    }
    ss_free(rs);
    ss_free(emitted);
    rel_free_shell(L);
    rel_free(R);
    return out;
}

static Relation *op_subplan(IRNode *n, Env *outer, int is_view) {
    Relation *in = exec_node(n->sub, is_view ? NULL : outer);
    Relation *out = rel_new(in->ncols);
    for (int i = 0; i < in->ncols; i++)
        rel_set_col(out, i, n->alias, i < n->nout ? n->out_names[i] : in->cols[i].name, in->cols[i].dtype, in->cols[i].dlen);
    for (int i = 0; i < in->nrows; i++) rel_add_row(out, in->rows[i], 0);
    rel_free_shell(in);
    return out;
}

Relation *exec_node(IRNode *n, Env *outer) {
    switch (n->op) {
    case IR_LOAD: {
        TableSym *t = table_or_die(n->table);
        return load_table(t, n->alias ? n->alias : t->name, n->cols, n->ncols);
    }
    case IR_VIEW: return op_subplan(n, outer, 1);
    case IR_DERIVED: return op_subplan(n, outer, 0);
    case IR_DUAL: {
        Relation *r = rel_new(0);
        rel_add_row(r, row_new(0), 0);
        return r;
    }
    case IR_VALUES: {
        Node *rows = n->ast;
        int nc = rows->nkids ? rows->kids[0]->nkids : 0;
        Relation *r = rel_new(nc);
        for (int i = 0; i < nc; i++) {
            char nm[16];
            snprintf(nm, sizeof nm, "col%d", i + 1);
            rel_set_col(r, i, NULL, nm, rows->kids[0]->kids[i]->dtype, 0);
        }
        for (int k = 0; k < rows->nkids; k++) {
            Value *row = row_new(nc);
            for (int i = 0; i < nc; i++) row[i] = eval_expr(rows->kids[k]->kids[i], outer);
            rel_add_row(r, row, 0);
        }
        return r;
    }
    case IR_FILTER: return op_filter(n, outer);
    case IR_JOIN: return op_join(n, outer);
    case IR_GROUP: return op_group(n, outer);
    case IR_AGGREGATE: return op_aggregate(n, outer);
    case IR_PROJECT: return op_project(n, outer);
    case IR_DISTINCT: return op_distinct(exec_node(n->left, outer));
    case IR_SORT: return op_sort(n, outer);
    case IR_LIMIT: return op_limit(n, outer);
    case IR_SETOP: return op_setop(n, outer);
    default:
        exec_error(NULL, "operator %s cannot produce rows", ir_op_name(n->op));
    }
}

Relation *exec_subquery(Node *q, Env *outer) {
    if (!q->plan) exec_error(q, "internal: subquery was not compiled");
    return exec_node((IRNode *)q->plan, outer);
}

/* ======================================================================
 *  Result printing
 * ====================================================================== */
static int utf8_len(const char *s) {
    int n = 0;
    for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

void print_relation(Relation *r, double secs) {
    int nc = r->ncols;
    if (nc == 0) { printf("(%d row%s, no columns)\n", r->nrows, r->nrows == 1 ? "" : "s"); return; }
    int *w = xcalloc((size_t)nc, sizeof(int));
    int *right = xcalloc((size_t)nc, sizeof(int));
    char ***cells = xcalloc((size_t)(r->nrows ? r->nrows : 1), sizeof(char **));
    for (int c = 0; c < nc; c++) {
        w[c] = utf8_len(r->cols[c].name);
        right[c] = dt_is_numeric(r->cols[c].dtype);
    }
    for (int i = 0; i < r->nrows; i++) {
        cells[i] = xcalloc((size_t)nc, sizeof(char *));
        for (int c = 0; c < nc; c++) {
            Value v = r->rows[i][c];
            if (v.k == V_FLOAT && (r->cols[c].dtype == DT_DECIMAL || r->cols[c].dtype == DT_NUMERIC)) {
                TableSym *dummy = NULL; (void)dummy;
                cells[i][c] = v_to_text(v, r->cols[c].dtype);
            } else cells[i][c] = v_to_text(v, r->cols[c].dtype);
            if (v.k == V_INT || v.k == V_FLOAT) right[c] = 1;
            int l = utf8_len(cells[i][c]);
            if (l > w[c]) w[c] = l;
        }
    }
    SB line; sb_init(&line);
    sb_addc(&line, '+');
    for (int c = 0; c < nc; c++) { for (int k = 0; k < w[c] + 2; k++) sb_addc(&line, '-'); sb_addc(&line, '+'); }
    char *sep = sb_take(&line);
    printf("%s\n|", sep);
    for (int c = 0; c < nc; c++) printf(" %s%s%s%*s |", COL_BOLD, r->cols[c].name, COL_RESET, w[c] - utf8_len(r->cols[c].name), "");
    printf("\n%s\n", sep);
    for (int i = 0; i < r->nrows; i++) {
        printf("|");
        for (int c = 0; c < nc; c++) {
            int pad = w[c] - utf8_len(cells[i][c]);
            int isnull = r->rows[i][c].k == V_NULL;
            if (right[c]) printf(" %*s%s%s%s |", pad, "", isnull ? COL_DIM : "", cells[i][c], isnull ? COL_RESET : "");
            else printf(" %s%s%s%*s |", isnull ? COL_DIM : "", cells[i][c], isnull ? COL_RESET : "", pad, "");
            free(cells[i][c]);
        }
        printf("\n");
        free(cells[i]);
    }
    if (r->nrows) printf("%s\n", sep);
    printf("%d row%s in set", r->nrows, r->nrows == 1 ? "" : "s");
    if (secs >= 0) printf(" (%.3f sec)", secs);
    printf("\n");
    free(sep);
    free(cells);
    free(w);
    free(right);
}

static void okmsg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("%sQuery OK:%s %s\n", COL_GREEN, COL_RESET, buf);
}

/* ======================================================================
 *  Constraint enforcement helpers
 * ====================================================================== */
static Relation *table_rel_shape(TableSym *t) {
    Relation *r = rel_new(t->ncols);
    for (int i = 0; i < t->ncols; i++) rel_set_col(r, i, t->name, t->cols[i].name, t->cols[i].type, t->cols[i].len);
    return r;
}

static char *key_desc(TableSym *t, int ncols, char **cols, Value *row) {
    SB b; sb_init(&b);
    for (int i = 0; i < ncols; i++) {
        char *v = v_to_sql(row[column_index(t, cols[i])]);
        sb_add(&b, "%s%s = %s", i ? ", " : "", cols[i], v);
        free(v);
    }
    return sb_take(&b);
}

static int uses_col(Node *e, const char *c) {
    if (!e) return 0;
    if (e->type == N_COLREF && ci_eq(e->s2, c)) return 1;
    for (int i = 0; i < e->nkids; i++) if (uses_col(e->kids[i], c)) return 1;
    return 0;
}

static void check_row_basic(TableSym *t, Value *row, Relation *shape, const char *what, int rowno) {
    for (int c = 0; c < t->ncols; c++)
        if (t->cols[c].not_null && row[c].k == V_NULL)
            exec_error(NULL, "NOT NULL constraint failed: %s.%s cannot be NULL (%s row %d)", t->name, t->cols[c].name, what, rowno);
    Env env = { shape, row, 0, NULL };
    for (int k = 0; k < t->nchecks; k++) {
        Value v = eval_expr(t->checks[k].expr, &env);
        int tr = v_truth(v);
        v_free(&v);
        if (tr == 0) {
            SB b; sb_init(&b);
            for (int c = 0; c < t->ncols; c++) {
                if (!uses_col(t->checks[k].expr, t->cols[c].name)) continue;
                char *vs = v_to_sql(row[c]);
                sb_add(&b, "%s%s = %s", b.len ? ", " : "", t->cols[c].name, vs);
                free(vs);
            }
            char *d = sb_take(&b);
            exec_error(NULL, "CHECK constraint failed: (%s) is FALSE for %s (%s row %d)", t->checks[k].sql, d, what, rowno);
        }
    }
}

/* unique keys = PRIMARY KEY, UNIQUE constraints and UNIQUE indexes */
typedef struct { const char *label; int ncols; char **cols; int primary; } UKey;
static int unique_keys(TableSym *t, UKey *out, int max) {
    int n = 0;
    for (int k = 0; k < t->nkeys && n < max; k++) {
        out[n].label = t->keys[k].kind == C_PRIMARY ? "PRIMARY KEY" : "UNIQUE constraint";
        out[n].ncols = t->keys[k].ncols;
        out[n].cols = t->keys[k].cols;
        out[n].primary = t->keys[k].kind == C_PRIMARY;
        n++;
    }
    int ni;
    char **names = ht_sorted_keys(g_cat.indexes, &ni);
    for (int i = 0; i < ni && n < max; i++) {
        IndexSym *ix = ht_get(g_cat.indexes, names[i]);
        if (ix->unique && ci_eq(ix->table, t->name)) {
            out[n].label = "UNIQUE index";
            out[n].ncols = ix->ncols;
            out[n].cols = ix->cols;
            out[n].primary = 0;
            n++;
        }
    }
    free(names);
    return n;
}

static char *key_of(TableSym *t, int ncols, char **cols, Value *row, int *has_null) {
    SB b; sb_init(&b);
    *has_null = 0;
    for (int i = 0; i < ncols; i++) {
        Value v = row[column_index(t, cols[i])];
        if (v.k == V_NULL) *has_null = 1;
        key_value(&b, v);
    }
    return sb_take(&b);
}

/* verify all unique keys over the final state `rows` (n rows) */
static void check_unique_state(TableSym *t, Value **rows, int n, const char *what) {
    UKey keys[32];
    int nk = unique_keys(t, keys, 32);
    for (int k = 0; k < nk; k++) {
        StrSet *s = ss_new(n);
        for (int i = 0; i < n; i++) {
            int hn;
            char *key = key_of(t, keys[k].ncols, keys[k].cols, rows[i], &hn);
            if (!hn) {
                if (ss_get(s, key)) {
                    char *d = key_desc(t, keys[k].ncols, keys[k].cols, rows[i]);
                    free(key);
                    ss_free(s);
                    exec_error(NULL, "%s violated on '%s': duplicate value (%s) (%s)", keys[k].label, t->name, d, what);
                }
                ss_add(s, key);
            }
            free(key);
        }
        ss_free(s);
    }
}

/* child rows (fk columns) must exist in parent state */
static void check_fk_child(TableSym *t, Value **rows, int n, Value **self_state, int self_n, const char *what) {
    for (int f = 0; f < t->nfks; f++) {
        FKSym *fk = &t->fks[f];
        TableSym *p = ht_get(g_cat.tables, fk->ref_table);
        if (!p) continue;
        StrSet *s = ss_new(64);
        if (ci_eq(p->name, t->name)) {
            for (int i = 0; i < self_n; i++) {
                int hn;
                char *k = key_of(p, fk->ncols, fk->ref_cols, self_state[i], &hn);
                ss_add(s, k);
                free(k);
            }
        } else {
            Relation *pr = load_table(p, p->name, NULL, -1);
            for (int i = 0; i < pr->nrows; i++) {
                int hn;
                char *k = key_of(p, fk->ncols, fk->ref_cols, pr->rows[i], &hn);
                ss_add(s, k);
                free(k);
            }
            rel_free(pr);
        }
        for (int i = 0; i < n; i++) {
            int hn;
            char *k = key_of(t, fk->ncols, fk->cols, rows[i], &hn);
            if (!hn && !ss_get(s, k)) {
                char *d = key_desc(t, fk->ncols, fk->cols, rows[i]);
                free(k);
                ss_free(s);
                exec_error(NULL, "FOREIGN KEY constraint failed: %s(%s) has no matching row in %s(%s) (%s)",
                           t->name, d, p->name, fk->ref_cols[0], what);
            }
            free(k);
        }
        ss_free(s);
    }
}

/* rows of `t` that disappear or change key: no child may still reference them */
static void check_fk_parent(TableSym *t, Value **old_rows, int nold, Value **final_rows, int nfinal, const char *what) {
    int nt;
    char **names = ht_sorted_keys(g_cat.tables, &nt);
    for (int ti = 0; ti < nt; ti++) {
        TableSym *c = ht_get(g_cat.tables, names[ti]);
        for (int f = 0; f < c->nfks; f++) {
            FKSym *fk = &c->fks[f];
            if (!ci_eq(fk->ref_table, t->name)) continue;
            /* keys that still exist after the change */
            StrSet *alive = ss_new(nfinal);
            for (int i = 0; i < nfinal; i++) {
                int hn;
                char *k = key_of(t, fk->ncols, fk->ref_cols, final_rows[i], &hn);
                ss_add(alive, k);
                free(k);
            }
            /* keys referenced by the child table */
            StrSet *refd = ss_new(64);
            Relation *cr = NULL;
            Value **crows;
            int ncr;
            if (ci_eq(c->name, t->name)) { crows = final_rows; ncr = nfinal; }
            else { cr = load_table(c, c->name, NULL, -1); crows = cr->rows; ncr = cr->nrows; }
            for (int i = 0; i < ncr; i++) {
                int hn;
                char *k = key_of(c, fk->ncols, fk->cols, crows[i], &hn);
                if (!hn) ss_add(refd, k);
                free(k);
            }
            for (int i = 0; i < nold; i++) {
                int hn;
                char *k = key_of(t, fk->ncols, fk->ref_cols, old_rows[i], &hn);
                if (!hn && ss_get(refd, k) && !ss_get(alive, k)) {
                    char *d = key_desc(t, fk->ncols, fk->ref_cols, old_rows[i]);
                    free(k);
                    exec_error(NULL, "FOREIGN KEY constraint failed: %s row (%s) is still referenced by %s.%s (%s)",
                               t->name, d, c->name, fk->cols[0], what);
                }
                free(k);
            }
            ss_free(alive);
            ss_free(refd);
            if (cr) rel_free(cr);
        }
    }
    free(names);
}

/* ======================================================================
 *  DML
 * ====================================================================== */
static void exec_insert(IRNode *n) {
    TableSym *t = table_or_die(n->table);
    Node *cl = n->ast->kids[0];
    int map[256], nvals;
    if (cl) { nvals = cl->nkids; for (int i = 0; i < nvals; i++) map[i] = column_index(t, cl->kids[i]->s1); }
    else { nvals = t->ncols; for (int i = 0; i < nvals; i++) map[i] = i; }

    Relation *src = exec_node(n->left, NULL);
    Relation *shape = table_rel_shape(t);
    Relation *existing = load_table(t, t->name, NULL, -1);
    int total = existing->nrows + src->nrows;
    Value **state = xcalloc((size_t)(total ? total : 1), sizeof(Value *));
    for (int i = 0; i < existing->nrows; i++) state[i] = existing->rows[i];
    Value **newrows = xcalloc((size_t)(src->nrows ? src->nrows : 1), sizeof(Value *));

    for (int r = 0; r < src->nrows; r++) {
        Value *row = row_new(t->ncols);
        for (int c = 0; c < t->ncols; c++) {
            int vi = -1;
            for (int k = 0; k < nvals; k++) if (map[k] == c) vi = k;
            Value v;
            if (vi >= 0) v = v_copy(src->rows[r][vi]);
            else if (t->cols[c].default_expr) v = eval_expr(t->cols[c].default_expr, NULL);
            else v = v_null();
            char e[256];
            if (!coerce_value(v, t->cols[c].type, t->cols[c].len, t->cols[c].scale, &row[c], e, sizeof e))
                exec_error(NULL, "row %d, column '%s': %s", r + 1, t->cols[c].name, e);
            v_free(&v);
        }
        check_row_basic(t, row, shape, "INSERT", r + 1);
        newrows[r] = row;
        state[existing->nrows + r] = row;
    }
    check_unique_state(t, state, total, "INSERT");
    check_fk_child(t, newrows, src->nrows, state, total, "INSERT");

    SB b; sb_init(&b);
    char *tq = qid(t->name);
    sb_add(&b, "INSERT INTO %s (", tq);
    free(tq);
    for (int c = 0; c < t->ncols; c++) { char *q = qid(t->cols[c].name); sb_add(&b, "%s%s", c ? ", " : "", q); free(q); }
    sb_add(&b, ") VALUES (");
    for (int c = 0; c < t->ncols; c++) sb_add(&b, "%s?", c ? ", " : "");
    sb_add(&b, ")");
    char *sql = sb_take(&b);
    sqlite3_stmt *st = prepare(sql);
    free(sql);
    for (int r = 0; r < src->nrows; r++) {
        for (int c = 0; c < t->ncols; c++) bind_value(st, c + 1, newrows[r][c]);
        step_done(st);
    }
    sqlite3_finalize(st);
    okmsg("%d row%s inserted into '%s'.", src->nrows, src->nrows == 1 ? "" : "s", t->name);
    txn_log("INSERT INTO %s  (%d row%s)", t->name, src->nrows, src->nrows == 1 ? "" : "s");
    for (int r = 0; r < src->nrows; r++) free_row(newrows[r], t->ncols);
    free(newrows);
    free(state);
    rel_free(existing);
    rel_free(src);
    rel_free(shape);
}

static void exec_update(IRNode *n) {
    TableSym *t = table_or_die(n->table);
    Node *al = n->ast->kids[0];
    Relation *target = exec_node(n->left, NULL);   /* rows to change, with rowids */
    Relation *shape = table_rel_shape(t);
    int na = al->nkids;
    int *ci = xcalloc((size_t)na, sizeof(int));
    for (int i = 0; i < na; i++) ci[i] = column_index(t, al->kids[i]->s1);

    Value **newrows = xcalloc((size_t)(target->nrows ? target->nrows : 1), sizeof(Value *));
    for (int r = 0; r < target->nrows; r++) {
        Value *row = copy_row(target->rows[r], t->ncols);
        Env env = { target, target->rows[r], 0, NULL };
        for (int i = 0; i < na; i++) {
            Value v = eval_expr(al->kids[i]->kids[0], &env);
            char e[256];
            Value cv;
            if (!coerce_value(v, t->cols[ci[i]].type, t->cols[ci[i]].len, t->cols[ci[i]].scale, &cv, e, sizeof e))
                exec_error(al->kids[i], "column '%s': %s", t->cols[ci[i]].name, e);
            v_free(&v);
            v_free(&row[ci[i]]);
            row[ci[i]] = cv;
        }
        check_row_basic(t, row, shape, "UPDATE", r + 1);
        newrows[r] = row;
    }
    /* final state of the whole table */
    Relation *all = load_table(t, t->name, NULL, -1);
    Value **state = xcalloc((size_t)(all->nrows ? all->nrows : 1), sizeof(Value *));
    for (int i = 0; i < all->nrows; i++) {
        state[i] = all->rows[i];
        for (int r = 0; r < target->nrows; r++) if (target->rowids[r] == all->rowids[i]) state[i] = newrows[r];
    }
    check_unique_state(t, state, all->nrows, "UPDATE");
    check_fk_child(t, newrows, target->nrows, state, all->nrows, "UPDATE");
    check_fk_parent(t, target->rows, target->nrows, state, all->nrows, "UPDATE");

    SB b; sb_init(&b);
    char *tq = qid(t->name);
    sb_add(&b, "UPDATE %s SET ", tq);
    free(tq);
    for (int i = 0; i < na; i++) { char *q = qid(t->cols[ci[i]].name); sb_add(&b, "%s%s = ?", i ? ", " : "", q); free(q); }
    sb_add(&b, " WHERE rowid = ?");
    char *sql = sb_take(&b);
    sqlite3_stmt *st = prepare(sql);
    free(sql);
    for (int r = 0; r < target->nrows; r++) {
        for (int i = 0; i < na; i++) bind_value(st, i + 1, newrows[r][ci[i]]);
        sqlite3_bind_int64(st, na + 1, target->rowids[r]);
        step_done(st);
    }
    sqlite3_finalize(st);
    okmsg("%d row%s updated in '%s'.", target->nrows, target->nrows == 1 ? "" : "s", t->name);
    txn_log("UPDATE %s  (%d row%s)", t->name, target->nrows, target->nrows == 1 ? "" : "s");
    for (int r = 0; r < target->nrows; r++) free_row(newrows[r], t->ncols);
    free(newrows);
    free(state);
    free(ci);
    rel_free(all);
    rel_free(target);
    rel_free(shape);
}

static void exec_delete(IRNode *n) {
    TableSym *t = table_or_die(n->table);
    Relation *target = exec_node(n->left, NULL);
    Relation *all = load_table(t, t->name, NULL, -1);
    Value **remain = xcalloc((size_t)(all->nrows ? all->nrows : 1), sizeof(Value *));
    int nremain = 0;
    for (int i = 0; i < all->nrows; i++) {
        int del = 0;
        for (int r = 0; r < target->nrows && !del; r++) if (target->rowids[r] == all->rowids[i]) del = 1;
        if (!del) remain[nremain++] = all->rows[i];
    }
    check_fk_parent(t, target->rows, target->nrows, remain, nremain, "DELETE");
    char *tq = qid(t->name);
    char *sql = xsprintf("DELETE FROM %s WHERE rowid = ?", tq);
    free(tq);
    sqlite3_stmt *st = prepare(sql);
    free(sql);
    for (int r = 0; r < target->nrows; r++) {
        sqlite3_bind_int64(st, 1, target->rowids[r]);
        step_done(st);
    }
    sqlite3_finalize(st);
    okmsg("%d row%s deleted from '%s'.", target->nrows, target->nrows == 1 ? "" : "s", t->name);
    txn_log("DELETE FROM %s  (%d row%s)", t->name, target->nrows, target->nrows == 1 ? "" : "s");
    free(remain);
    rel_free(all);
    rel_free(target);
}

/* ======================================================================
 *  DDL / DCL / TCL / utility commands
 * ====================================================================== */
static void save_catalog(void) {
    if (symtab_save(g_db)) exec_error(NULL, "could not save the catalog: %s", sqlite3_errmsg(g_db));
}

/* after a change, every view must still compile */
static void revalidate_views(const char *what) {
    int n;
    char **names = ht_sorted_keys(g_cat.views, &n);
    for (int i = 0; i < n; i++) {
        ViewSym *v = ht_get(g_cat.views, names[i]);
        v->ncols = 0;
        if (!sem_view_query(v, 0)) {
            char nm[MAX_NAME];
            snprintf(nm, sizeof nm, "%s", v->name);
            free(names);
            exec_error(NULL, "%s would break view '%s' which depends on it (DROP VIEW %s first)", what, nm, nm);
        }
    }
    free(names);
}

static void rename_in_expr(Node *e, const char *old, const char *nw) {
    if (!e) return;
    if (e->type == N_COLREF && ci_eq(e->s2, old)) e->s2 = xstrdup(nw);
    for (int i = 0; i < e->nkids; i++) rename_in_expr(e->kids[i], old, nw);
}

static void replace_table_symbol(TableSym *old, TableSym *nt) {
    delete_table(old->name);
    insert_table(nt);
}

static void exec_alter(Node *s) {
    TableSym *t = table_or_die(s->s1);
    TableSym *nt = s->info;
    Node *a = s->kids[0];
    char *tq = qid(t->name);
    char *sql;
    switch (a->op) {
    case A_ADD: {
        ColumnSym *c = &nt->cols[nt->ncols - 1];
        char *cq = qid(c->name);
        sql = xsprintf("ALTER TABLE %s ADD COLUMN %s", tq, cq);
        storage_exec(sql);
        free(sql);
        Relation *rows = load_table(t, t->name, NULL, -1);
        Value dv = c->default_expr ? eval_expr(c->default_expr, NULL) : v_null();
        Value cv;
        char e[256];
        if (!coerce_value(dv, c->type, c->len, c->scale, &cv, e, sizeof e)) exec_error(NULL, "DEFAULT for '%s': %s", c->name, e);
        if (rows->nrows && c->not_null && cv.k == V_NULL)
            exec_error(NULL, "cannot add NOT NULL column '%s' without a DEFAULT: table '%s' already has %d row(s)", c->name, t->name, rows->nrows);
        if (cv.k != V_NULL && rows->nrows) {
            sql = xsprintf("UPDATE %s SET %s = ?", tq, cq);
            sqlite3_stmt *st = prepare(sql);
            free(sql);
            bind_value(st, 1, cv);
            step_done(st);
            sqlite3_finalize(st);
        }
        /* validate existing rows against new constraints */
        Relation *after = load_table(nt, nt->name, NULL, -1);
        Relation *shape = table_rel_shape(nt);
        for (int i = 0; i < after->nrows; i++) check_row_basic(nt, after->rows[i], shape, "existing", i + 1);
        replace_table_symbol(t, nt);
        check_unique_state(nt, after->rows, after->nrows, "ALTER TABLE ADD");
        rel_free(after); rel_free(shape); rel_free(rows);
        free(cq);
        { char *ts = column_type_str(c); okmsg("column '%s' %s added to '%s'.", c->name, ts, nt->name); free(ts); }
        break;
    }
    case A_DROP: {
        char *cq = qid(a->s1);
        sql = xsprintf("ALTER TABLE %s DROP COLUMN %s", tq, cq);
        storage_exec(sql);
        free(sql);
        free(cq);
        replace_table_symbol(t, nt);
        revalidate_views("dropping this column");
        okmsg("column '%s' dropped from '%s'.", a->s1, nt->name);
        break;
    }
    case A_MODIFY: {
        const char *cname = a->kids[0]->s1;
        int ci = column_index(nt, cname);
        ColumnSym *c = &nt->cols[ci];
        Relation *rows = load_table(t, t->name, NULL, -1);
        int oci = column_index(t, cname);
        char *cq = qid(c->name);
        sql = xsprintf("UPDATE %s SET %s = ? WHERE rowid = ?", tq, cq);
        sqlite3_stmt *st = prepare(sql);
        free(sql);
        int converted = 0;
        for (int i = 0; i < rows->nrows; i++) {
            Value out;
            char e[256];
            if (!coerce_value(rows->rows[i][oci], c->type, c->len, c->scale, &out, e, sizeof e)) {
                char *pk = NULL;
                KeySym *k = table_primary_key(t);
                if (k) pk = key_desc(t, k->ncols, k->cols, rows->rows[i]);
                sqlite3_finalize(st);
                exec_error(NULL, "cannot convert existing data of column '%s'%s%s%s: %s", cname,
                           pk ? " (row " : "", pk ? pk : "", pk ? ")" : "", e);
            }
            v_free(&rows->rows[i][oci]);
            rows->rows[i][oci] = out;
            bind_value(st, 1, out);
            sqlite3_bind_int64(st, 2, rows->rowids[i]);
            step_done(st);
            converted++;
        }
        sqlite3_finalize(st);
        free(cq);
        Relation *shape = table_rel_shape(nt);
        for (int i = 0; i < rows->nrows; i++) check_row_basic(nt, rows->rows[i], shape, "existing", i + 1);
        replace_table_symbol(t, nt);
        check_unique_state(nt, rows->rows, rows->nrows, "ALTER TABLE MODIFY");
        revalidate_views("changing this column");
        rel_free(shape);
        rel_free(rows);
        char *ts = column_type_str(c);
        okmsg("column '%s' is now %s (%d existing value%s converted).", cname, ts, converted, converted == 1 ? "" : "s");
        free(ts);
        break;
    }
    case A_RENAME_COL: {
        char *oq = qid(a->s1), *nq = qid(a->s2);
        sql = xsprintf("ALTER TABLE %s RENAME COLUMN %s TO %s", tq, oq, nq);
        storage_exec(sql);
        free(sql); free(oq); free(nq);
        for (int k = 0; k < nt->nchecks; k++) {
            rename_in_expr(nt->checks[k].expr, a->s1, a->s2);
            nt->checks[k].sql = expr_to_sql(nt->checks[k].expr);
        }
        replace_table_symbol(t, nt);
        int n;
        char **names = ht_sorted_keys(g_cat.indexes, &n);
        for (int i = 0; i < n; i++) {
            IndexSym *ix = ht_get(g_cat.indexes, names[i]);
            if (!ci_eq(ix->table, nt->name)) continue;
            for (int j = 0; j < ix->ncols; j++) if (ci_eq(ix->cols[j], a->s1)) ix->cols[j] = xstrdup(a->s2);
        }
        free(names);
        names = ht_sorted_keys(g_cat.tables, &n);
        for (int i = 0; i < n; i++) {
            TableSym *o = ht_get(g_cat.tables, names[i]);
            if (o == nt) continue;
            for (int f = 0; f < o->nfks; f++)
                if (ci_eq(o->fks[f].ref_table, nt->name))
                    for (int j = 0; j < o->fks[f].ncols; j++)
                        if (ci_eq(o->fks[f].ref_cols[j], a->s1)) o->fks[f].ref_cols[j] = xstrdup(a->s2);
        }
        free(names);
        revalidate_views("renaming this column");
        okmsg("column '%s' renamed to '%s' in '%s'.", a->s1, a->s2, nt->name);
        break;
    }
    case A_RENAME_TABLE: {
        char *nq = qid(a->s2);
        sql = xsprintf("ALTER TABLE %s RENAME TO %s", tq, nq);
        storage_exec(sql);
        free(sql); free(nq);
        char old[MAX_NAME];
        snprintf(old, sizeof old, "%s", t->name);
        delete_table(old);
        insert_table(nt);
        int n;
        char **names = ht_sorted_keys(g_cat.indexes, &n);
        for (int i = 0; i < n; i++) {
            IndexSym *ix = ht_get(g_cat.indexes, names[i]);
            if (ci_eq(ix->table, old)) snprintf(ix->table, MAX_NAME, "%s", a->s2);
        }
        free(names);
        names = ht_sorted_keys(g_cat.tables, &n);
        for (int i = 0; i < n; i++) {
            TableSym *o = ht_get(g_cat.tables, names[i]);
            for (int f = 0; f < o->nfks; f++) if (ci_eq(o->fks[f].ref_table, old)) o->fks[f].ref_table = xstrdup(a->s2);
        }
        free(names);
        rename_object_privileges(old, a->s2);
        revalidate_views("renaming this table");
        okmsg("table '%s' renamed to '%s'.", old, a->s2);
        break;
    }
    }
    free(tq);
    save_catalog();
    txn_log("ALTER TABLE %s", s->s1);
}

static void show_names(const char *title, HashTable *h) {
    int n;
    char **keys = ht_sorted_keys(h, &n);
    Relation *r = rel_new(1);
    rel_set_col(r, 0, NULL, title, DT_VARCHAR, 0);
    for (int i = 0; i < n; i++) { Value *row = row_new(1); row[0] = v_str(keys[i]); rel_add_row(r, row, 0); }
    print_relation(r, -1);
    rel_free(r);
    free(keys);
}

static void show_grants(const char *user) {
    int n;
    char **keys = ht_sorted_keys(g_cat.users, &n);
    int shown = 0;
    for (int i = 0; i < n; i++) {
        UserSym *u = ht_get(g_cat.users, keys[i]);
        if (user && !ci_eq(user, u->name)) continue;
        shown++;
        printf("%s%s%s%s\n", COL_BOLD, u->name, COL_RESET, u->is_admin ? "  (administrator: ALL privileges on every object)" : "");
        if (!u->is_admin && !u->npriv) printf(" %s(no privileges)\n", tree_branch(1));
        for (int j = 0; j < u->npriv; j++) {
            int m = u->privs[j].privs;
            const char *nm[] = { "SELECT", "INSERT", "UPDATE", "DELETE" };
            int cnt = 0, total = 0;
            for (int b = 0; b < 4; b++) if (m & (1 << b)) total++;
            for (int b = 0; b < 4; b++) {
                if (!(m & (1 << b))) continue;
                cnt++;
                int last = (j == u->npriv - 1) && cnt == total;
                printf(" %s%s -> %s\n", tree_branch(last), nm[b], u->privs[j].object);
            }
        }
    }
    free(keys);
    if (!shown) printf("(no users)\n");
}

static void describe(const char *name) {
    TableSym *t = ht_get(g_cat.tables, name);
    if (t) {
        Relation *r = rel_new(6);
        const char *h[] = { "column", "type", "null", "key", "default", "extra" };
        for (int i = 0; i < 6; i++) rel_set_col(r, i, NULL, h[i], DT_VARCHAR, 0);
        for (int c = 0; c < t->ncols; c++) {
            ColumnSym *col = &t->cols[c];
            Value *row = row_new(6);
            row[0] = v_str(col->name);
            char *ts = column_type_str(col);
            row[1] = v_str(ts);
            free(ts);
            row[2] = v_str(col->not_null ? "NO" : "YES");
            const char *key = col->primary ? "PRI" : col->unique ? "UNI" : "";
            for (int f = 0; f < t->nfks && !*key; f++)
                for (int j = 0; j < t->fks[f].ncols; j++) if (ci_eq(t->fks[f].cols[j], col->name)) key = "FK";
            row[3] = v_str(key);
            row[4] = col->default_sql ? v_str(col->default_sql) : v_null();
            SB b; sb_init(&b);
            for (int f = 0; f < t->nfks; f++)
                for (int j = 0; j < t->fks[f].ncols; j++)
                    if (ci_eq(t->fks[f].cols[j], col->name)) sb_add(&b, "-> %s(%s)", t->fks[f].ref_table, t->fks[f].ref_cols[j]);
            row[5] = v_str(b.s ? b.s : "");
            free(sb_take(&b));
            rel_add_row(r, row, 0);
        }
        print_relation(r, -1);
        rel_free(r);
        for (int k = 0; k < t->nchecks; k++) printf("CHECK (%s)\n", t->checks[k].sql);
        for (int k = 0; k < t->nkeys; k++)
            if (t->keys[k].ncols > 1) {
                printf("%s (", t->keys[k].kind == C_PRIMARY ? "PRIMARY KEY" : "UNIQUE");
                for (int j = 0; j < t->keys[k].ncols; j++) printf("%s%s", j ? ", " : "", t->keys[k].cols[j]);
                printf(")\n");
            }
        return;
    }
    ViewSym *v = ht_get(g_cat.views, name);
    if (v) {
        sem_view_columns(v);
        printf("VIEW %s AS %s\n", v->name, v->sql);
        Relation *r = rel_new(2);
        rel_set_col(r, 0, NULL, "column", DT_VARCHAR, 0);
        rel_set_col(r, 1, NULL, "type", DT_VARCHAR, 0);
        for (int i = 0; i < v->ncols; i++) {
            Value *row = row_new(2);
            row[0] = v_str(v->colnames[i]);
            row[1] = v_str(dtype_name(v->coltypes[i]));
            rel_add_row(r, row, 0);
        }
        print_relation(r, -1);
        rel_free(r);
    }
}

static char *db_path_of(const char *name) { return xsprintf("%s/%s.db", g_dbdir, name); }

int db_file_exists(const char *name) {
    char *p = db_path_of(name);
    struct stat sb;
    int r = stat(p, &sb) == 0;
    free(p);
    return r;
}

static int open_db(const char *name) {
    char *p = db_path_of(name);
    sqlite3 *db;
    if (sqlite3_open(p, &db) != SQLITE_OK) {
        fprintf(stderr, "cannot open database %s: %s\n", p, sqlite3_errmsg(db));
        free(p);
        return -1;
    }
    free(p);
    if (g_db) sqlite3_close(g_db);
    g_db = db;
    snprintf(g_dbname, sizeof g_dbname, "%s", name);
    txn_init(g_db);
    symtab_init(name);
    symtab_load(g_db);
    return 0;
}

int executor_open(const char *dir, const char *dbname) {
    snprintf(g_dbdir, sizeof g_dbdir, "%s", dir);
    mkdir(g_dbdir, 0755);
    return open_db(dbname);
}
void executor_close(void) {
    if (txn_active()) txn_rollback();
    if (g_db) sqlite3_close(g_db);
    g_db = NULL;
}

static void list_databases(void) {
    Relation *r = rel_new(2);
    rel_set_col(r, 0, NULL, "database", DT_VARCHAR, 0);
    rel_set_col(r, 1, NULL, "in use", DT_VARCHAR, 0);
    char *pat = xsprintf("%s/*.db", g_dbdir);
    glob_t g;
    if (glob(pat, 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            const char *base = strrchr(g.gl_pathv[i], '/');
            base = base ? base + 1 : g.gl_pathv[i];
            char *name = xstrndup(base, strlen(base) - 3);
            Value *row = row_new(2);
            row[0] = v_str(name);
            row[1] = v_str(ci_eq(name, g_dbname) ? "*" : "");
            rel_add_row(r, row, 0);
            free(name);
        }
        globfree(&g);
    }
    free(pat);
    print_relation(r, -1);
    rel_free(r);
}

static void exec_command(Node *s) {
    char *sql, *q, *q2;
    switch (s->type) {
    case N_CREATE_TABLE: {
        TableSym *t = s->info;
        if (!t) { okmsg("table '%s' already exists, nothing done.", s->s1); return; }
        SB b; sb_init(&b);
        q = qid(t->name);
        sb_add(&b, "CREATE TABLE %s (", q);
        free(q);
        for (int i = 0; i < t->ncols; i++) { q = qid(t->cols[i].name); sb_add(&b, "%s%s", i ? ", " : "", q); free(q); }
        sb_add(&b, ")");
        sql = sb_take(&b);
        storage_exec(sql);
        free(sql);
        insert_table(t);
        save_catalog();
        okmsg("table '%s' created (%d column%s).", t->name, t->ncols, t->ncols == 1 ? "" : "s");
        txn_log("CREATE TABLE %s", t->name);
        return;
    }
    case N_CREATE_VIEW: {
        ViewSym *v = xcalloc(1, sizeof(ViewSym));
        snprintf(v->name, MAX_NAME, "%s", s->s1);
        v->sql = query_to_sql(s->kids[0]);
        QInfo *qi = s->kids[0]->info;
        v->ncols = qi->nout;
        v->colnames = qi->out_names;
        v->coltypes = qi->out_types;
        insert_view(v);
        save_catalog();
        okmsg("view '%s' created.", v->name);
        txn_log("CREATE VIEW %s", v->name);
        return;
    }
    case N_CREATE_INDEX: {
        TableSym *t = table_or_die(s->s2);
        IndexSym *ix = xcalloc(1, sizeof(IndexSym));
        snprintf(ix->name, MAX_NAME, "%s", s->s1);
        snprintf(ix->table, MAX_NAME, "%s", t->name);
        ix->unique = s->flag;
        ix->ncols = s->kids[0]->nkids;
        ix->cols = xcalloc((size_t)ix->ncols, sizeof(char *));
        for (int i = 0; i < ix->ncols; i++) ix->cols[i] = xstrdup(t->cols[column_index(t, s->kids[0]->kids[i]->s1)].name);
        insert_index(ix);
        if (ix->unique) {
            Relation *rows = load_table(t, t->name, NULL, -1);
            check_unique_state(t, rows->rows, rows->nrows, "CREATE UNIQUE INDEX on existing data");
            rel_free(rows);
        }
        SB b; sb_init(&b);
        q = qid(ix->name); q2 = qid(t->name);
        sb_add(&b, "CREATE INDEX %s ON %s (", q, q2);
        free(q); free(q2);
        for (int i = 0; i < ix->ncols; i++) { q = qid(ix->cols[i]); sb_add(&b, "%s%s", i ? ", " : "", q); free(q); }
        sb_add(&b, ")");
        sql = sb_take(&b);
        storage_exec(sql);
        free(sql);
        save_catalog();
        okmsg("%sindex '%s' created on %s.", ix->unique ? "unique " : "", ix->name, t->name);
        txn_log("CREATE INDEX %s", ix->name);
        return;
    }
    case N_ALTER_TABLE: exec_alter(s); return;
    case N_DROP_TABLE: {
        TableSym *t = ht_get(g_cat.tables, s->s1);
        if (!t) { okmsg("table '%s' does not exist, nothing done.", s->s1); return; }
        char name[MAX_NAME];
        snprintf(name, sizeof name, "%s", t->name);
        int n;
        char **names = ht_sorted_keys(g_cat.indexes, &n);
        for (int i = 0; i < n; i++) {
            IndexSym *ix = ht_get(g_cat.indexes, names[i]);
            if (ci_eq(ix->table, name)) delete_index(ix->name);
        }
        free(names);
        q = qid(name);
        sql = xsprintf("DROP TABLE %s", q);
        storage_exec(sql);
        free(sql); free(q);
        delete_table(name);
        drop_object_privileges(name);
        revalidate_views("dropping this table");
        save_catalog();
        okmsg("table '%s' dropped.", name);
        txn_log("DROP TABLE %s", name);
        return;
    }
    case N_DROP_VIEW:
        if (!ht_get(g_cat.views, s->s1)) { okmsg("view '%s' does not exist, nothing done.", s->s1); return; }
        delete_view(s->s1);
        drop_object_privileges(s->s1);
        revalidate_views("dropping this view");
        save_catalog();
        okmsg("view '%s' dropped.", s->s1);
        txn_log("DROP VIEW %s", s->s1);
        return;
    case N_DROP_INDEX:
        if (!ht_get(g_cat.indexes, s->s1)) { okmsg("index '%s' does not exist, nothing done.", s->s1); return; }
        q = qid(s->s1);
        sql = xsprintf("DROP INDEX IF EXISTS %s", q);
        storage_exec(sql);
        free(sql); free(q);
        delete_index(s->s1);
        save_catalog();
        okmsg("index '%s' dropped.", s->s1);
        txn_log("DROP INDEX %s", s->s1);
        return;
    case N_TRUNCATE: {
        TableSym *t = table_or_die(s->s1);
        Relation *all = load_table(t, t->name, NULL, -1);
        check_fk_parent(t, all->rows, all->nrows, NULL, 0, "TRUNCATE");
        q = qid(t->name);
        sql = xsprintf("DELETE FROM %s", q);
        storage_exec(sql);
        free(sql); free(q);
        okmsg("table '%s' truncated (%d row%s removed).", t->name, all->nrows, all->nrows == 1 ? "" : "s");
        txn_log("TRUNCATE %s", t->name);
        rel_free(all);
        return;
    }
    case N_CREATE_DATABASE: {
        if (db_file_exists(s->s1)) { okmsg("database '%s' already exists, nothing done.", s->s1); return; }
        char *p = db_path_of(s->s1);
        sqlite3 *db;
        if (sqlite3_open(p, &db) != SQLITE_OK) { free(p); exec_error(NULL, "cannot create database file"); }
        sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS __sqlc_catalog(seq INTEGER, kind TEXT, name TEXT, def TEXT)", NULL, NULL, NULL);
        sqlite3_close(db);
        okmsg("database '%s' created. Switch to it with: USE %s;", s->s1, s->s1);
        free(p);
        return;
    }
    case N_DROP_DATABASE: {
        if (!db_file_exists(s->s1)) { okmsg("database '%s' does not exist, nothing done.", s->s1); return; }
        char *p = db_path_of(s->s1);
        if (remove(p) != 0) { free(p); exec_error(NULL, "could not remove database file"); }
        free(p);
        okmsg("database '%s' dropped.", s->s1);
        return;
    }
    case N_USE:
        if (open_db(s->s1)) exec_error(NULL, "could not open database '%s'", s->s1);
        snprintf(g_current_user, MAX_NAME, "admin");
        okmsg("using database '%s' (%d table%s).", g_dbname, g_cat.tables->count, g_cat.tables->count == 1 ? "" : "s");
        return;
    case N_GRANT: case N_REVOKE: {
        int m = 0;
        for (int i = 0; i < s->kids[0]->nkids; i++) m |= s->kids[0]->kids[i]->op;
        char *ps = priv_mask_str(m);
        const char *obj = ht_get(g_cat.tables, s->s1) ? ((TableSym *)ht_get(g_cat.tables, s->s1))->name : s->s1;
        for (int i = 0; i < s->kids[1]->nkids; i++) {
            const char *u = s->kids[1]->kids[i]->s1;
            if (s->type == N_GRANT) {
                if (!ht_get(g_cat.users, u)) insert_user(u);
                grant_privilege(u, obj, m);
                okmsg("granted %s on '%s' to '%s'.", ps, obj, u);
            } else {
                int removed = revoke_privilege(u, obj, m);
                if (!removed) printf("%s[WARNING]%s user '%s' had none of %s on '%s'\n", COL_YELLOW, COL_RESET, u, ps, obj);
                else {
                    char *rs = priv_mask_str(removed);
                    okmsg("revoked %s on '%s' from '%s'.", rs, obj, u);
                    free(rs);
                }
            }
        }
        save_catalog();
        txn_log("%s %s ON %s", s->type == N_GRANT ? "GRANT" : "REVOKE", ps, obj);
        free(ps);
        return;
    }
    case N_CREATE_USER:
        insert_user(s->s1);
        save_catalog();
        okmsg("user '%s' created (no privileges yet).", s->s1);
        txn_log("CREATE USER %s", s->s1);
        return;
    case N_DROP_USER:
        delete_user(s->s1);
        save_catalog();
        okmsg("user '%s' dropped.", s->s1);
        txn_log("DROP USER %s", s->s1);
        return;
    case N_CONNECT: {
        UserSym *u = ht_get(g_cat.users, s->s1);
        snprintf(g_current_user, MAX_NAME, "%s", u ? u->name : s->s1);
        okmsg("connected as '%s'.", g_current_user);
        return;
    }
    case N_BEGIN:
        if (txn_begin(0)) exec_error(s, "could not start a transaction");
        okmsg("transaction %d started (state: ACTIVE).", g_txn.id);
        return;
    case N_COMMIT: {
        if (!txn_active()) { okmsg("nothing to commit (autocommit mode)."); return; }
        int n = g_txn.nops;
        if (txn_commit()) exec_error(s, "COMMIT failed");
        okmsg("transaction %d COMMITTED - %d operation%s made permanent.", g_txn.id, n, n == 1 ? "" : "s");
        return;
    }
    case N_ROLLBACK: {
        if (!txn_active()) { okmsg("nothing to roll back (autocommit mode)."); return; }
        int n = g_txn.nops;
        if (txn_rollback()) exec_error(s, "ROLLBACK failed");
        symtab_load(g_db);
        okmsg("transaction %d ROLLED BACK - %d operation%s undone.", g_txn.id, n, n == 1 ? "" : "s");
        return;
    }
    case N_SAVEPOINT:
        if (!txn_active()) {
            if (txn_begin(1)) exec_error(s, "could not start a transaction");
            printf("%sNOTE:%s transaction %d started implicitly.\n", COL_CYAN, COL_RESET, g_txn.id);
        }
        if (txn_has_savepoint(s->s1)) {
            printf("%sNOTE:%s savepoint '%s' re-created at the current position.\n", COL_CYAN, COL_RESET, s->s1);
            txn_release(s->s1);
        }
        if (txn_savepoint(s->s1)) exec_error(s, "could not create savepoint");
        okmsg("savepoint '%s' created in transaction %d.", s->s1, g_txn.id);
        return;
    case N_ROLLBACK_TO: {
        int before = g_txn.nops;
        if (txn_rollback_to(s->s1)) exec_error(s, "savepoint '%s' does not exist", s->s1);
        symtab_load(g_db);
        int undone = before - g_txn.nops;
        okmsg("rolled back to savepoint '%s' - %d operation%s undone, transaction %d still ACTIVE.",
              s->s1, undone, undone == 1 ? "" : "s", g_txn.id);
        return;
    }
    case N_RELEASE:
        if (txn_release(s->s1)) exec_error(s, "savepoint '%s' does not exist", s->s1);
        okmsg("savepoint '%s' released.", s->s1);
        return;
    case N_SHOW:
        switch (s->op) {
        case SH_TABLES: show_names("tables", g_cat.tables); break;
        case SH_VIEWS: show_names("views", g_cat.views); break;
        case SH_INDEXES: {
            int n;
            char **keys = ht_sorted_keys(g_cat.indexes, &n);
            Relation *r = rel_new(4);
            const char *h[] = { "index", "table", "columns", "unique" };
            for (int i = 0; i < 4; i++) rel_set_col(r, i, NULL, h[i], DT_VARCHAR, 0);
            for (int i = 0; i < n; i++) {
                IndexSym *ix = ht_get(g_cat.indexes, keys[i]);
                Value *row = row_new(4);
                row[0] = v_str(ix->name);
                row[1] = v_str(ix->table);
                SB b; sb_init(&b);
                for (int j = 0; j < ix->ncols; j++) sb_add(&b, "%s%s", j ? ", " : "", ix->cols[j]);
                char *cs = sb_take(&b);
                row[2] = v_str(cs);
                free(cs);
                row[3] = v_str(ix->unique ? "YES" : "NO");
                rel_add_row(r, row, 0);
            }
            print_relation(r, -1);
            rel_free(r);
            free(keys);
            break;
        }
        case SH_USERS: show_names("users", g_cat.users); break;
        case SH_DATABASES: list_databases(); break;
        case SH_GRANTS: show_grants(s->s1); break;
        case SH_TRANSACTION: txn_print_status(); break;
        }
        return;
    case N_DESCRIBE: describe(s->s1); return;
    default:
        exec_error(s, "statement cannot be executed");
    }
}

/* ======================================================================
 *  Statement driver
 * ====================================================================== */
static int needs_atomic(Node *s) {
    switch (s->type) {
    case N_INSERT: case N_UPDATE: case N_DELETE:
    case N_CREATE_TABLE: case N_CREATE_VIEW: case N_CREATE_INDEX: case N_ALTER_TABLE:
    case N_DROP_TABLE: case N_DROP_VIEW: case N_DROP_INDEX: case N_TRUNCATE:
    case N_GRANT: case N_REVOKE: case N_CREATE_USER: case N_DROP_USER:
        return 1;
    default: return 0;
    }
}

int execute_plan(Plan *p) {
    Node *s = p->stmt;
    if (s->type == N_EXPLAIN) s = s->kids[0];
    volatile int atomic = needs_atomic(s);
    volatile int started = 0;
    jmp_buf jb;
    g_exec_jmp = &jb;
    if (setjmp(jb)) {
        g_exec_jmp = NULL;
        diag_error(ERR_EXECUTION, g_exec_errline, g_exec_errcol, "%s", g_exec_errmsg);
        if (started) {
            stmt_rollback();
            symtab_load(g_db);
            printf("%sStatement rolled back%s - no changes were made%s.\n", COL_YELLOW, COL_RESET,
                   txn_active() ? " (the transaction is still ACTIVE)" : "");
        }
        return -1;
    }
    if (atomic) { if (stmt_begin()) exec_error(NULL, "could not start statement"); started = 1; }
    clock_t t0 = clock();
    IRNode *r = p->root;
    switch (r->op) {
    case IR_INSERT: exec_insert(r); break;
    case IR_UPDATE: exec_update(r); break;
    case IR_DELETE: exec_delete(r); break;
    case IR_COMMAND: exec_command(r->ast); break;
    default: {
        Relation *res = exec_node(r, NULL);
        double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
        print_relation(res, secs);
        rel_free(res);
    }
    }
    if (started) stmt_commit();
    g_exec_jmp = NULL;
    return 0;
}
