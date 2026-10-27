/*
 * eval.c - values, relations and the expression evaluator
 */
#include "eval.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>

jmp_buf *g_exec_jmp = NULL;
char     g_exec_errmsg[1024];
int      g_exec_errline, g_exec_errcol;

void exec_error(Node *where, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_exec_errmsg, sizeof g_exec_errmsg, fmt, ap);
    va_end(ap);
    g_exec_errline = where ? where->line : 0;
    g_exec_errcol = where ? where->col : 0;
    if (g_exec_jmp) longjmp(*g_exec_jmp, 1);
    fprintf(stderr, "fatal: %s\n", g_exec_errmsg);
    exit(3);
}

/* ======================================================================
 *  Values
 * ====================================================================== */
Value v_null(void) { Value v = {V_NULL, 0, 0, NULL}; return v; }
Value v_int(long long i) { Value v = {V_INT, i, 0, NULL}; return v; }
Value v_float(double f) { Value v = {V_FLOAT, 0, f, NULL}; return v; }
Value v_str(const char *s) { Value v = {V_STR, 0, 0, xstrdup(s ? s : "")}; return v; }
Value v_bool(int b) { Value v = {V_BOOL, b ? 1 : 0, 0, NULL}; return v; }
Value v_copy(Value v) { if (v.k == V_STR) v.s = xstrdup(v.s); return v; }
void  v_free(Value *v) { if (v->k == V_STR) free(v->s); v->s = NULL; v->k = V_NULL; }
int   v_is_null(Value v) { return v.k == V_NULL; }

int v_truth(Value v) {
    switch (v.k) {
    case V_NULL: return -1;
    case V_BOOL: case V_INT: return v.i != 0;
    case V_FLOAT: return v.f != 0.0;
    case V_STR: return v.s[0] != 0;
    }
    return 0;
}
double v_num(Value v) {
    switch (v.k) {
    case V_INT: case V_BOOL: return (double)v.i;
    case V_FLOAT: return v.f;
    case V_STR: return strtod(v.s, NULL);
    default: return 0;
    }
}
static int is_num(Value v) { return v.k == V_INT || v.k == V_FLOAT || v.k == V_BOOL; }

int v_compare(Value a, Value b) {
    if (is_num(a) && is_num(b)) {
        if (a.k != V_FLOAT && b.k != V_FLOAT) return (a.i > b.i) - (a.i < b.i);
        double x = v_num(a), y = v_num(b);
        return (x > y) - (x < y);
    }
    if (a.k == V_STR && b.k == V_STR) {
        int c = strcmp(a.s, b.s);
        return (c > 0) - (c < 0);
    }
    /* mixed: compare textual representations */
    char *x = v_to_text(a, 0), *y = v_to_text(b, 0);
    int c = strcmp(x, y);
    free(x); free(y);
    return (c > 0) - (c < 0);
}
int v_same(Value a, Value b) {
    if (a.k == V_NULL || b.k == V_NULL) return a.k == b.k;
    return v_compare(a, b) == 0;
}

char *v_to_text(Value v, int dtype) {
    char buf[64];
    switch (v.k) {
    case V_NULL: return xstrdup("NULL");
    case V_INT: return xsprintf("%lld", v.i);
    case V_BOOL: return xstrdup(v.i ? "TRUE" : "FALSE");
    case V_FLOAT: fmt_double(v.f, buf, sizeof buf); (void)dtype; return xstrdup(buf);
    case V_STR: return xstrdup(v.s);
    }
    return xstrdup("?");
}
char *v_to_sql(Value v) {
    if (v.k == V_STR) {
        SB b; sb_init(&b);
        sb_addc(&b, '\'');
        for (const char *p = v.s; *p; p++) { if (*p == '\'') sb_addc(&b, '\''); sb_addc(&b, *p); }
        sb_addc(&b, '\'');
        return sb_take(&b);
    }
    return v_to_text(v, 0);
}

/* SQL LIKE with % (any sequence) and _ (any single char); case-insensitive */
int like_match(const char *s, const char *p) {
    while (*p) {
        if (*p == '%') {
            while (*p == '%') p++;
            if (!*p) return 1;
            for (; *s; s++) if (like_match(s, p)) return 1;
            return 0;
        }
        if (!*s) return 0;
        if (*p != '_' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return 0;
        p++; s++;
    }
    return *s == 0;
}

/* ======================================================================
 *  Relations
 * ====================================================================== */
static int next_sid = 1;

Relation *rel_new(int ncols) {
    Relation *r = xcalloc(1, sizeof(Relation));
    r->ncols = ncols;
    r->cols = xcalloc((size_t)(ncols ? ncols : 1), sizeof(ColInfo));
    r->sid = next_sid++;
    return r;
}
void rel_set_col(Relation *r, int i, const char *qual, const char *name, int dtype, long long len) {
    r->cols[i].qual = xstrdup(qual);
    r->cols[i].name = xstrdup(name);
    r->cols[i].dtype = dtype;
    r->cols[i].dlen = len;
}
Value *row_new(int n) { return xcalloc((size_t)(n ? n : 1), sizeof(Value)); }
void rel_add_row(Relation *r, Value *row, long long rowid) {
    if (r->nrows == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 16;
        r->rows = xrealloc(r->rows, sizeof(Value *) * (size_t)r->cap);
        r->rowids = xrealloc(r->rowids, sizeof(long long) * (size_t)r->cap);
    }
    r->rows[r->nrows] = row;
    r->rowids[r->nrows] = rowid;
    r->nrows++;
}
void rel_free(Relation *r) {
    if (!r) return;
    for (int i = 0; i < r->nrows; i++) {
        for (int j = 0; j < r->ncols; j++) v_free(&r->rows[i][j]);
        free(r->rows[i]);
    }
    for (int j = 0; j < r->ncols; j++) { free(r->cols[j].qual); free(r->cols[j].name); }
    free(r->cols);
    free(r->rows);
    free(r->rowids);
    free(r->group_start);
    free(r);
}
int rel_find_col(Relation *r, const char *qual, const char *name) {
    if (qual) {
        for (int i = 0; i < r->ncols; i++)
            if (r->cols[i].qual && ci_eq(r->cols[i].qual, qual) && ci_eq(r->cols[i].name, name)) return i;
    }
    int found = -1;
    for (int i = 0; i < r->ncols; i++)
        if (ci_eq(r->cols[i].name, name)) { if (found >= 0 && qual) return -1; found = i; if (!qual) break; }
    return found;
}

/* ======================================================================
 *  Type coercion (used when storing values into typed columns)
 * ====================================================================== */
static int parse_ll(const char *s, long long *out) {
    char *end;
    errno = 0;
    while (isspace((unsigned char)*s)) s++;
    long long v = strtoll(s, &end, 10);
    while (isspace((unsigned char)*end)) end++;
    if (errno || end == s || *end) return 0;
    *out = v;
    return 1;
}
static int parse_d(const char *s, double *out) {
    char *end;
    while (isspace((unsigned char)*s)) s++;
    double v = strtod(s, &end);
    while (isspace((unsigned char)*end)) end++;
    if (end == s || *end) return 0;
    *out = v;
    return 1;
}
static int valid_date_str(const char *s) {
    int y, m, d; char extra;
    if (strlen(s) != 10 || sscanf(s, "%4d-%2d-%2d%c", &y, &m, &d, &extra) != 3) return 0;
    static const int md[] = {31,29,31,30,31,30,31,31,30,31,30,31};
    if (m < 1 || m > 12 || d < 1 || d > md[m - 1]) return 0;
    if (m == 2 && d == 29 && !((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 0;
    return 1;
}
static int valid_time_str(const char *s) {
    int h, m, sec; char extra;
    if (strlen(s) != 8 || sscanf(s, "%2d:%2d:%2d%c", &h, &m, &sec, &extra) != 3) return 0;
    return h >= 0 && h < 24 && m >= 0 && m < 60 && sec >= 0 && sec < 60;
}
static int valid_datetime_str(const char *s) {
    if (strlen(s) == 10) return valid_date_str(s);
    if (strlen(s) != 19 || (s[10] != ' ' && s[10] != 'T')) return 0;
    char d[11], t[9];
    memcpy(d, s, 10); d[10] = 0;
    memcpy(t, s + 11, 8); t[8] = 0;
    return valid_date_str(d) && valid_time_str(t);
}

int coerce_value(Value in, int dt, long long len, int scale, Value *out, char *err, size_t n) {
    *out = v_null();
    if (in.k == V_NULL) return 1;
    char *txt;
    if (dt_is_integer(dt)) {
        long long v;
        if (in.k == V_INT || in.k == V_BOOL) v = in.i;
        else if (in.k == V_FLOAT) v = (long long)llround(in.f);
        else if (!parse_ll(in.s, &v)) {
            double d;
            if (parse_d(in.s, &d)) v = (long long)llround(d);
            else { snprintf(err, n, "cannot convert '%s' to %s", in.s, dtype_name(dt)); return 0; }
        }
        if (dt == DT_SMALLINT && (v < -32768 || v > 32767)) {
            snprintf(err, n, "value %lld is out of range for SMALLINT", v);
            return 0;
        }
        if (dt == DT_INT && (v < -2147483648LL || v > 2147483647LL)) {
            snprintf(err, n, "value %lld is out of range for INT (use BIGINT)", v);
            return 0;
        }
        *out = v_int(v);
        return 1;
    }
    if (dt_is_numeric(dt)) {
        double d;
        if (in.k == V_STR) {
            if (!parse_d(in.s, &d)) { snprintf(err, n, "cannot convert '%s' to %s", in.s, dtype_name(dt)); return 0; }
        } else d = v_num(in);
        if ((dt == DT_DECIMAL || dt == DT_NUMERIC) && scale >= 0) {
            double p = pow(10, scale);
            d = round(d * p) / p;
        }
        *out = v_float(d);
        return 1;
    }
    if (dt_is_string(dt)) {
        txt = v_to_text(in, 0);
        if (len > 0 && (long long)strlen(txt) > len && dt != DT_TEXT) {
            snprintf(err, n, "value '%s' is too long for %s(%lld) (length %zu)", txt, dtype_name(dt), len, strlen(txt));
            free(txt);
            return 0;
        }
        out->k = V_STR;
        out->s = txt;
        return 1;
    }
    if (dt == DT_DATE || dt == DT_TIME || dt == DT_DATETIME) {
        if (in.k != V_STR) { snprintf(err, n, "cannot convert a number to %s", dtype_name(dt)); return 0; }
        int ok = dt == DT_DATE ? valid_date_str(in.s) : dt == DT_TIME ? valid_time_str(in.s) : valid_datetime_str(in.s);
        if (!ok) {
            snprintf(err, n, "'%s' is not a valid %s (expected %s)", in.s, dtype_name(dt),
                     dt == DT_DATE ? "YYYY-MM-DD" : dt == DT_TIME ? "HH:MM:SS" : "YYYY-MM-DD HH:MM:SS");
            return 0;
        }
        *out = v_str(in.s);
        return 1;
    }
    if (dt == DT_BOOLEAN) {
        if (in.k == V_BOOL) { *out = in; return 1; }
        if (in.k == V_INT && (in.i == 0 || in.i == 1)) { *out = v_bool((int)in.i); return 1; }
        if (in.k == V_STR) {
            if (ci_eq(in.s, "true") || strcmp(in.s, "1") == 0) { *out = v_bool(1); return 1; }
            if (ci_eq(in.s, "false") || strcmp(in.s, "0") == 0) { *out = v_bool(0); return 1; }
        }
        txt = v_to_text(in, 0);
        snprintf(err, n, "cannot convert '%s' to BOOLEAN", txt);
        free(txt);
        return 0;
    }
    *out = v_copy(in);
    return 1;
}

/* ======================================================================
 *  Expression evaluation
 * ====================================================================== */
static Value eval_col(Node *e, Env *env) {
    Env *en = env;
    for (int d = e->depth; d > 0 && en; d--) en = en->outer;
    if (!en || !en->rel) exec_error(e, "column '%s' is not available here", e->s2);
    Relation *r = en->rel;
    int idx;
    if (e->bind_sid == r->sid) idx = e->bind_idx;
    else {
        idx = rel_find_col(r, e->rqual ? e->rqual : e->s1, e->s2);
        e->bind_sid = r->sid;
        e->bind_idx = idx;
    }
    if (idx < 0) exec_error(e, "column '%s' is not available at this point of the plan", e->s2);
    return v_copy(en->row[idx]);
}

static Value arith(Node *e, Value a, Value b) {
    if (a.k == V_NULL || b.k == V_NULL) return v_null();
    if (!is_num(a) || !is_num(b)) exec_error(e, "operator '%s' needs numeric operands", op_symbol(e->op));
    int ints = a.k != V_FLOAT && b.k != V_FLOAT;
    if (ints) {
        long long x = a.i, y = b.i;
        switch (e->op) {
        case O_ADD: return v_int(x + y);
        case O_SUB: return v_int(x - y);
        case O_MUL: return v_int(x * y);
        case O_DIV: if (y == 0) exec_error(e, "division by zero"); return v_int(x / y);
        case O_MOD: if (y == 0) exec_error(e, "division by zero (modulo)"); return v_int(x % y);
        }
    }
    double x = v_num(a), y = v_num(b);
    switch (e->op) {
    case O_ADD: return v_float(x + y);
    case O_SUB: return v_float(x - y);
    case O_MUL: return v_float(x * y);
    case O_DIV: if (y == 0) exec_error(e, "division by zero"); return v_float(x / y);
    case O_MOD: if (y == 0) exec_error(e, "division by zero (modulo)"); return v_float(fmod(x, y));
    }
    return v_null();
}

static Value compare_op(int op, Value a, Value b) {
    if (a.k == V_NULL || b.k == V_NULL) return v_null();
    int c = v_compare(a, b);
    switch (op) {
    case O_EQ: return v_bool(c == 0);
    case O_NE: return v_bool(c != 0);
    case O_LT: return v_bool(c < 0);
    case O_GT: return v_bool(c > 0);
    case O_LE: return v_bool(c <= 0);
    case O_GE: return v_bool(c >= 0);
    }
    return v_null();
}

static Value tri(int t) { return t < 0 ? v_null() : v_bool(t); }
static Value tri_not(Value v) { int t = v_truth(v); return t < 0 ? v_null() : v_bool(!t); }

static Relation *subquery_result(Node *holder, Node *q, Env *env) {
    if (holder->cached) return (Relation *)holder->cache;
    Relation *r = exec_subquery(q, env);
    if (!q->correlated) { holder->cache = r; holder->cached = 1; }
    return r;
}
static void release_subq(Node *holder, Relation *r) { if (!holder->cached) rel_free(r); }

static Value eval_func(Node *e, Env *env) {
    const char *f = e->s1;
    int n = e->nkids;
    Value a = n > 0 ? eval_expr(e->kids[0], env) : v_null();
    if (!strcmp(f, "COALESCE") || !strcmp(f, "IFNULL")) {
        if (a.k != V_NULL) return a;
        for (int i = 1; i < n; i++) {
            Value v = eval_expr(e->kids[i], env);
            if (v.k != V_NULL) return v;
        }
        return v_null();
    }
    if (!strcmp(f, "CONCAT")) {
        SB b; sb_init(&b);
        for (int i = 0; i < n; i++) {
            Value v = i == 0 ? a : eval_expr(e->kids[i], env);
            if (v.k != V_NULL) { char *t = v_to_text(v, 0); sb_add(&b, "%s", t); free(t); }
            v_free(&v);
        }
        Value r = {V_STR, 0, 0, sb_take(&b)};
        return r;
    }
    if (a.k == V_NULL) return a;
    if (!strcmp(f, "UPPER") || !strcmp(f, "LOWER")) {
        char *t = v_to_text(a, 0);
        for (char *p = t; *p; p++) *p = (char)(f[0] == 'U' ? toupper((unsigned char)*p) : tolower((unsigned char)*p));
        v_free(&a);
        Value r = {V_STR, 0, 0, t};
        return r;
    }
    if (!strcmp(f, "LENGTH")) {
        char *t = v_to_text(a, 0);
        Value r = v_int((long long)strlen(t));
        free(t);
        v_free(&a);
        return r;
    }
    if (!strcmp(f, "ABS")) {
        if (a.k == V_INT) return v_int(a.i < 0 ? -a.i : a.i);
        return v_float(fabs(v_num(a)));
    }
    if (!strcmp(f, "ROUND")) {
        int d = 0;
        if (n > 1) { Value dv = eval_expr(e->kids[1], env); d = (int)v_num(dv); }
        double p = pow(10, d);
        double r = round(v_num(a) * p) / p;
        if (d == 0 && a.k == V_INT) return a;
        return v_float(r);
    }
    exec_error(e, "unknown function %s", f);
}

Value eval_expr(Node *e, Env *env) {
    if (!e) return v_null();
    if (env && env->post_agg && e->slot >= 0) return v_copy(env->row[e->slot]);
    Value a, b, r;
    switch (e->type) {
    case N_INT: return v_int(e->ival);
    case N_FLOAT: return v_float(e->fval);
    case N_STRING: case N_DATE: return v_str(e->s1);
    case N_NULL: return v_null();
    case N_BOOL: return v_bool((int)e->ival);
    case N_COLREF: return eval_col(e, env);

    case N_BINOP:
        if (e->op == O_AND) {
            a = eval_expr(e->kids[0], env);
            int ta = v_truth(a);
            if (ta == 0) return v_bool(0);
            b = eval_expr(e->kids[1], env);
            int tb = v_truth(b);
            v_free(&a); v_free(&b);
            if (tb == 0) return v_bool(0);
            return (ta < 0 || tb < 0) ? v_null() : v_bool(1);
        }
        if (e->op == O_OR) {
            a = eval_expr(e->kids[0], env);
            int ta = v_truth(a);
            if (ta == 1) return v_bool(1);
            b = eval_expr(e->kids[1], env);
            int tb = v_truth(b);
            v_free(&a); v_free(&b);
            if (tb == 1) return v_bool(1);
            return (ta < 0 || tb < 0) ? v_null() : v_bool(0);
        }
        a = eval_expr(e->kids[0], env);
        b = eval_expr(e->kids[1], env);
        if (e->op >= O_EQ && e->op <= O_GE) r = compare_op(e->op, a, b);
        else r = arith(e, a, b);
        v_free(&a); v_free(&b);
        return r;

    case N_UNOP:
        a = eval_expr(e->kids[0], env);
        if (e->op == O_NOT) { r = tri_not(a); v_free(&a); return r; }
        if (a.k == V_NULL) return a;
        if (e->op == O_NEG) {
            if (a.k == V_INT) return v_int(-a.i);
            if (a.k == V_FLOAT) return v_float(-a.f);
            exec_error(e, "unary minus needs a numeric operand");
        }
        return a;

    case N_FUNC: return eval_func(e, env);

    case N_AGG:
        exec_error(e, "aggregate %s() used outside of an aggregation", agg_name(e->op));

    case N_BETWEEN: {
        a = eval_expr(e->kids[0], env);
        Value lo = eval_expr(e->kids[1], env), hi = eval_expr(e->kids[2], env);
        Value c1 = compare_op(O_GE, a, lo), c2 = compare_op(O_LE, a, hi);
        int t1 = v_truth(c1), t2 = v_truth(c2), t;
        if (t1 == 0 || t2 == 0) t = 0;
        else if (t1 < 0 || t2 < 0) t = -1;
        else t = 1;
        v_free(&a); v_free(&lo); v_free(&hi);
        if (e->flag && t >= 0) t = !t;
        return tri(t);
    }
    case N_IN_LIST: {
        a = eval_expr(e->kids[0], env);
        if (a.k == V_NULL) return v_null();
        int found = 0, saw_null = 0;
        Node *l = e->kids[1];
        for (int i = 0; i < l->nkids && !found; i++) {
            b = eval_expr(l->kids[i], env);
            if (b.k == V_NULL) saw_null = 1;
            else if (v_compare(a, b) == 0) found = 1;
            v_free(&b);
        }
        v_free(&a);
        int t = found ? 1 : (saw_null ? -1 : 0);
        if (e->flag && t >= 0) t = !t;
        return tri(t);
    }
    case N_IN_SUBQ: {
        a = eval_expr(e->kids[0], env);
        if (a.k == V_NULL) return v_null();
        Relation *sr = subquery_result(e, e->kids[1], env);
        int found = 0, saw_null = 0;
        for (int i = 0; i < sr->nrows && !found; i++) {
            Value x = sr->rows[i][0];
            if (x.k == V_NULL) saw_null = 1;
            else if (v_compare(a, x) == 0) found = 1;
        }
        release_subq(e, sr);
        v_free(&a);
        int t = found ? 1 : (saw_null ? -1 : 0);
        if (e->flag && t >= 0) t = !t;
        return tri(t);
    }
    case N_LIKE: {
        a = eval_expr(e->kids[0], env);
        b = eval_expr(e->kids[1], env);
        if (a.k == V_NULL || b.k == V_NULL) { v_free(&a); v_free(&b); return v_null(); }
        char *s = v_to_text(a, 0), *p = v_to_text(b, 0);
        int m = like_match(s, p);
        free(s); free(p);
        v_free(&a); v_free(&b);
        return v_bool(e->flag ? !m : m);
    }
    case N_IS_NULL:
        a = eval_expr(e->kids[0], env);
        r = v_bool(e->flag ? a.k != V_NULL : a.k == V_NULL);
        v_free(&a);
        return r;
    case N_SUBQUERY: {
        Relation *sr = subquery_result(e, e->kids[0], env);
        if (sr->nrows > 1) {
            int n = sr->nrows;
            release_subq(e, sr);
            exec_error(e, "scalar subquery returned %d rows (expected at most 1)", n);
        }
        r = sr->nrows ? v_copy(sr->rows[0][0]) : v_null();
        release_subq(e, sr);
        return r;
    }
    case N_EXISTS: {
        Relation *sr = subquery_result(e, e->kids[0], env);
        int t = sr->nrows > 0;
        release_subq(e, sr);
        return v_bool(t);
    }
    default:
        exec_error(e, "cannot evaluate this expression");
    }
}

int eval_predicate(Node *e, Env *env) {
    if (!e) return 1;
    Value v = eval_expr(e, env);
    int t = v_truth(v);
    v_free(&v);
    return t == 1;
}
