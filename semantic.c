/*
 * semantic.c - semantic analyzer
 *
 * Output while analysing (in full mode) looks like:
 *
 *   [OK]   Table 'students' found (5 columns)
 *   [OK]   Column 'cgpa' -> students.cgpa : FLOAT
 *   [OK]   WHERE clause is a valid BOOLEAN condition
 *
 * Errors are reported as SEMANTIC ERROR with the source position.
 */
#include "semantic.h"
#include "diag.h"
#include "eval.h"
#include "lexer.h"
#include "transaction.h"
#include "executor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

int g_sem_verbose = 1;

static int n_errors = 0;       /* errors reported for the statement        */
static int quiet = 0;          /* >0 : analysing silently (views, catalog) */
static int quiet_errors = 0;
static int skip_priv = 0;      /* >0 : privilege checks disabled           */
static PtrVec logged;

/* ======================================================================
 *  Reporting helpers
 * ====================================================================== */
static void say(const char *tag, const char *color, const char *fmt, va_list ap) {
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    if (quiet || !g_sem_verbose) return;
    if (pv_contains_str(&logged, buf)) return;
    pv_push(&logged, xstrdup(buf));
    printf("  %s%-6s%s %s\n", color, tag, COL_RESET, buf);
}
static void ok(const char *fmt, ...) { va_list ap; va_start(ap, fmt); say("[OK]", COL_GREEN, fmt, ap); va_end(ap); }
static void note(const char *fmt, ...) { va_list ap; va_start(ap, fmt); say("[NOTE]", COL_CYAN, fmt, ap); va_end(ap); }

static void err(Node *at, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (quiet) { quiet_errors++; return; }
    n_errors++;
    diag_error(ERR_SEMANTIC, at ? at->line : 0, at ? at->col : 0, "%s", buf);
}
static void warn(Node *at, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (quiet) return;
    diag_warning(at ? at->line : 0, at ? at->col : 0, "%s", buf);
}

/* "did you mean ...?" using edit distance */
static int lev(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    int *d = xmalloc(sizeof(int) * (size_t)((la + 1) * (lb + 1)));
#define D(i, j) d[(i) * (lb + 1) + (j)]
    for (int i = 0; i <= la; i++) D(i, 0) = i;
    for (int j = 0; j <= lb; j++) D(0, j) = j;
    for (int i = 1; i <= la; i++)
        for (int j = 1; j <= lb; j++) {
            int c = (tolower((unsigned char)a[i - 1]) != tolower((unsigned char)b[j - 1]));
            int m = D(i - 1, j) + 1;
            if (D(i, j - 1) + 1 < m) m = D(i, j - 1) + 1;
            if (D(i - 1, j - 1) + c < m) m = D(i - 1, j - 1) + c;
            D(i, j) = m;
        }
    int r = D(la, lb);
#undef D
    free(d);
    return r;
}
static char suggestion[128];
static const char *hint_for(const char *name, char **cands, int n) {
    int best = 99;
    const char *bs = NULL;
    for (int i = 0; i < n; i++) {
        int d = lev(name, cands[i]);
        if (d < best) { best = d; bs = cands[i]; }
    }
    int limit = (int)strlen(name) <= 4 ? 1 : 2;
    if (bs && best <= limit) { snprintf(suggestion, sizeof suggestion, " (did you mean '%s'?)", bs); return suggestion; }
    return "";
}

char *canon_sql(Node *e) {
    g_unparse_resolved = 1;
    char *s = expr_to_sql(e);
    g_unparse_resolved = 0;
    return s;
}

/* ======================================================================
 *  Scopes and ranges (tables / aliases visible in a query)
 * ====================================================================== */
typedef struct {
    char      *name;       /* range name: alias or table name   */
    char      *object;     /* table / view name                 */
    int        ncols;
    char     **cols;
    int       *types;
    long long *lens;
} Range;

typedef struct Scope {
    Range        *r[32];
    int           n;
    struct Scope *parent;
    Node         *query;   /* N_QUERY owning this scope          */
} Scope;

static Range *range_new(const char *name, const char *object, int ncols) {
    Range *r = xcalloc(1, sizeof(Range));
    r->name = xstrdup(name);
    r->object = xstrdup(object);
    r->ncols = ncols;
    r->cols = xcalloc((size_t)(ncols ? ncols : 1), sizeof(char *));
    r->types = xcalloc((size_t)(ncols ? ncols : 1), sizeof(int));
    r->lens = xcalloc((size_t)(ncols ? ncols : 1), sizeof(long long));
    return r;
}
static Range *range_from_table(TableSym *t, const char *alias) {
    Range *r = range_new(alias ? alias : t->name, t->name, t->ncols);
    for (int i = 0; i < t->ncols; i++) {
        r->cols[i] = xstrdup(t->cols[i].name);
        r->types[i] = t->cols[i].type;
        r->lens[i] = t->cols[i].len;
    }
    return r;
}
static Range *range_from_qinfo(QInfo *qi, const char *name, const char *object) {
    Range *r = range_new(name, object, qi->nout);
    for (int i = 0; i < qi->nout; i++) {
        r->cols[i] = xstrdup(qi->out_names[i]);
        r->types[i] = qi->out_types[i];
    }
    return r;
}
static int range_col(Range *r, const char *c) {
    for (int i = 0; i < r->ncols; i++) if (ci_eq(r->cols[i], c)) return i;
    return -1;
}
static int scope_add(Scope *sc, Range *r, Node *at) {
    for (int i = 0; i < sc->n; i++)
        if (ci_eq(sc->r[i]->name, r->name)) {
            err(at, "table name '%s' is specified more than once in FROM (use an alias, e.g. for a self join)", r->name);
            return 0;
        }
    if (sc->n < 32) sc->r[sc->n++] = r;
    return 1;
}

/* ======================================================================
 *  Privileges
 * ====================================================================== */
static void need_priv(Node *at, const char *object, int priv) {
    if (skip_priv) return;
    char *pn = priv_mask_str(priv);
    if (has_privilege(g_current_user, object, priv))
        ok("Privilege: user '%s' may %s on '%s'", g_current_user, pn, object);
    else
        err(at, "permission denied: user '%s' does not have %s privilege on '%s'", g_current_user, pn, object);
    free(pn);
}
static int need_admin(Node *at, const char *what) {
    UserSym *u = ht_get(g_cat.users, g_current_user);
    if (u && u->is_admin) { ok("Privilege: user '%s' is an administrator and may run %s", u->name, what); return 1; }
    err(at, "permission denied: only an administrator can execute %s (current user: '%s')", what, g_current_user);
    return 0;
}

/* ======================================================================
 *  Types
 * ====================================================================== */
enum { CX_AGG_OK = 1, CX_NO_SUBQ = 2, CX_IN_AGG = 4 };
typedef struct { int flags; const char *clause; } Ctx;

static int tcat(int t) {
    if (dt_is_numeric(t)) return 1;
    if (dt_is_string(t)) return 2;
    if (dt_is_temporal(t)) return 3;
    if (dt_is_bool(t)) return 4;
    return 0;                         /* NULL / unknown */
}
static const char *tname(Node *e) {
    static char buf[4][48];
    static int k = 0;
    k = (k + 1) % 4;
    if (e->dtype == DT_NULL) return "NULL";
    if ((e->dtype == DT_VARCHAR || e->dtype == DT_CHAR) && e->dlen > 0 && e->type == N_COLREF)
        snprintf(buf[k], sizeof buf[k], "%s(%lld)", dtype_name(e->dtype), e->dlen);
    else snprintf(buf[k], sizeof buf[k], "%s", dtype_name(e->dtype));
    return buf[k];
}
/* can a string literal be used where a DATE/TIME is expected? */
static int string_literal_as(Node *lit, int dt) {
    if (lit->type != N_STRING) return 0;
    Value out;
    char e[128];
    int okv = coerce_value(v_str(lit->s1), dt, 0, 0, &out, e, sizeof e);
    v_free(&out);
    return okv;
}
static int comparable(Node *a, Node *b) {
    int ca = tcat(a->dtype), cb = tcat(b->dtype);
    if (!ca || !cb) return 1;
    if (ca == cb) return 1;
    if (ca == 3 && cb == 2) return string_literal_as(b, a->dtype);
    if (cb == 3 && ca == 2) return string_literal_as(a, b->dtype);
    if (ca == 4 && b->type == N_INT && (b->ival == 0 || b->ival == 1)) return 1;
    if (cb == 4 && a->type == N_INT && (a->ival == 0 || a->ival == 1)) return 1;
    return 0;
}
static void require_bool(Node *e, const char *clause) {
    if (!e) return;
    int c = tcat(e->dtype);
    if (c == 4 || e->dtype == DT_NULL || e->dtype == DT_UNKNOWN) {
        ok("%s is a valid BOOLEAN condition", clause);
        return;
    }
    err(e, "%s must be a BOOLEAN condition, but '%s' is %s", clause, expr_to_sql(e), tname(e));
}

static QInfo *an_query(Node *q, Scope *parent);
static int an_expr(Node *e, Scope *sc, Ctx cx);

/* ======================================================================
 *  Column resolution
 * ====================================================================== */
static void resolve_colref(Node *e, Scope *sc) {
    int depth = 0;
    for (Scope *s = sc; s; s = s->parent, depth++) {
        Range *hit = NULL;
        int hit_idx = -1, matches = 0;
        if (e->s1) {
            for (int i = 0; i < s->n; i++)
                if (ci_eq(s->r[i]->name, e->s1)) { hit = s->r[i]; break; }
            if (!hit) continue;
            hit_idx = range_col(hit, e->s2);
            if (hit_idx < 0) {
                err(e, "column '%s' does not exist in '%s'%s", e->s2, hit->name, hint_for(e->s2, hit->cols, hit->ncols));
                e->dtype = DT_UNKNOWN;
                return;
            }
        } else {
            for (int i = 0; i < s->n; i++) {
                int c = range_col(s->r[i], e->s2);
                if (c >= 0) {
                    matches++;
                    if (!hit) { hit = s->r[i]; hit_idx = c; }
                }
            }
            if (matches > 1) {
                char list[256] = "";
                for (int i = 0; i < s->n; i++)
                    if (range_col(s->r[i], e->s2) >= 0) {
                        strncat(list, list[0] ? ", " : "", sizeof list - strlen(list) - 1);
                        strncat(list, s->r[i]->name, sizeof list - strlen(list) - 1);
                    }
                err(e, "column reference '%s' is ambiguous (it exists in %s); qualify it, e.g. %s.%s",
                    e->s2, list, hit->name, e->s2);
                e->dtype = DT_UNKNOWN;
                return;
            }
            if (!hit) continue;
        }
        /* found */
        e->depth = depth;
        e->rqual = xstrdup(hit->name);
        e->dtype = hit->types[hit_idx];
        e->dlen = hit->lens[hit_idx];
        /* mark every query between here and the defining scope as correlated */
        for (Scope *x = sc; x && x != s; x = x->parent)
            if (x->query) x->query->correlated = 1;
        char tbuf[48];
        if ((e->dtype == DT_VARCHAR || e->dtype == DT_CHAR) && e->dlen > 0)
            snprintf(tbuf, sizeof tbuf, "%s(%lld)", dtype_name(e->dtype), e->dlen);
        else snprintf(tbuf, sizeof tbuf, "%s", dtype_name(e->dtype));
        symlog("lookup_column(%s, %s) -> %s", hit->object, hit->cols[hit_idx], tbuf);
        if (depth > 0)
            ok("Column '%s' -> %s.%s : %s (outer query reference: correlated subquery)", e->s2, hit->name, hit->cols[hit_idx], tbuf);
        else if (ci_eq(hit->name, hit->object))
            ok("Column '%s' -> %s.%s : %s", e->s2, hit->name, hit->cols[hit_idx], tbuf);
        else
            ok("Column '%s' -> %s.%s (%s) : %s", e->s2, hit->name, hit->cols[hit_idx], hit->object, tbuf);
        return;
    }
    e->dtype = DT_UNKNOWN;
    if (e->s1) {
        err(e, "unknown table or alias '%s' in column reference '%s.%s'", e->s1, e->s1, e->s2);
        return;
    }
    /* collect candidates for a suggestion */
    PtrVec c = {0};
    for (Scope *s = sc; s; s = s->parent)
        for (int i = 0; i < s->n; i++)
            for (int j = 0; j < s->r[i]->ncols; j++) pv_push(&c, s->r[i]->cols[j]);
    if (!sc || (sc->n == 0 && !sc->parent))
        err(e, "column '%s' cannot be used here (no table in scope)", e->s2);
    else if (sc->n == 1)
        err(e, "column '%s' does not exist in table '%s'%s", e->s2, sc->r[0]->object, hint_for(e->s2, (char **)c.items, c.n));
    else
        err(e, "column '%s' does not exist in any table of the FROM clause%s", e->s2, hint_for(e->s2, (char **)c.items, c.n));
    free(c.items);
}

/* ======================================================================
 *  Expressions
 * ====================================================================== */
static int check_subquery(Node *holder, Node *q, Scope *sc, Ctx cx, int want_cols) {
    if (cx.flags & CX_NO_SUBQ) {
        err(holder, "subqueries are not allowed in %s", cx.clause);
        return DT_UNKNOWN;
    }
    QInfo *qi = an_query(q, sc);
    if (!qi) return DT_UNKNOWN;
    if (want_cols && qi->nout != 1) {
        err(holder, "subquery must return exactly one column (it returns %d)", qi->nout);
        return DT_UNKNOWN;
    }
    ok("Subquery is valid%s", q->correlated ? " (correlated: re-evaluated for every outer row)" : " (uncorrelated)");
    return qi->nout ? qi->out_types[0] : DT_UNKNOWN;
}

typedef struct { const char *name; int min, max; int argcat; int ret; } FuncSig;
static const FuncSig funcs[] = {
    { "UPPER", 1, 1, 2, DT_VARCHAR }, { "LOWER", 1, 1, 2, DT_VARCHAR },
    { "LENGTH", 1, 1, 2, DT_INT },    { "ABS", 1, 1, 1, -1 },
    { "ROUND", 1, 2, 1, DT_FLOAT },   { "COALESCE", 1, 16, 0, -1 },
    { "IFNULL", 2, 2, 0, -1 },        { "CONCAT", 1, 16, 0, DT_VARCHAR },
};

static int an_expr(Node *e, Scope *sc, Ctx cx) {
    if (!e) return DT_UNKNOWN;
    int ta, tb;
    switch (e->type) {
    case N_INT: e->dtype = DT_INT; break;
    case N_FLOAT: e->dtype = DT_FLOAT; break;
    case N_STRING: e->dtype = DT_VARCHAR; e->dlen = (long long)strlen(e->s1); break;
    case N_DATE: e->dtype = DT_DATE; break;
    case N_NULL: e->dtype = DT_NULL; break;
    case N_BOOL: e->dtype = DT_BOOLEAN; break;
    case N_COLREF: resolve_colref(e, sc); break;

    case N_BINOP:
        ta = an_expr(e->kids[0], sc, cx);
        tb = an_expr(e->kids[1], sc, cx);
        if (e->op == O_AND || e->op == O_OR) {
            for (int i = 0; i < 2; i++) {
                int c = tcat(e->kids[i]->dtype);
                if (c && c != 4)
                    err(e->kids[i], "argument of %s must be BOOLEAN, but '%s' is %s", op_symbol(e->op),
                        expr_to_sql(e->kids[i]), tname(e->kids[i]));
            }
            e->dtype = DT_BOOLEAN;
        } else if (e->op >= O_EQ && e->op <= O_GE) {
            if (!comparable(e->kids[0], e->kids[1])) {
                if (tcat(ta) == 3 || tcat(tb) == 3)
                    err(e, "cannot compare %s with %s in '%s' (dates must be written as 'YYYY-MM-DD')",
                        tname(e->kids[0]), tname(e->kids[1]), expr_to_sql(e));
                else
                    err(e, "type mismatch: cannot compare %s with %s in '%s'", tname(e->kids[0]),
                        tname(e->kids[1]), expr_to_sql(e));
            }
            e->dtype = DT_BOOLEAN;
        } else {
            for (int i = 0; i < 2; i++) {
                int c = tcat(e->kids[i]->dtype);
                if (c && c != 1)
                    err(e->kids[i], "operator '%s' requires numeric operands, but '%s' is %s", op_symbol(e->op),
                        expr_to_sql(e->kids[i]), tname(e->kids[i]));
            }
            if (dt_is_integer(ta) && dt_is_integer(tb)) e->dtype = (ta == DT_BIGINT || tb == DT_BIGINT) ? DT_BIGINT : DT_INT;
            else if (ta == DT_NULL || tb == DT_NULL) e->dtype = DT_NULL;
            else if (tcat(ta) == 1 && tcat(tb) == 1) e->dtype = DT_FLOAT;
            else e->dtype = DT_UNKNOWN;
        }
        break;

    case N_UNOP:
        ta = an_expr(e->kids[0], sc, cx);
        if (e->op == O_NOT) {
            int c = tcat(ta);
            if (c && c != 4)
                err(e, "argument of NOT must be BOOLEAN, but '%s' is %s", expr_to_sql(e->kids[0]), tname(e->kids[0]));
            e->dtype = DT_BOOLEAN;
        } else {
            int c = tcat(ta);
            if (c && c != 1) err(e, "unary '%s' requires a numeric operand, but '%s' is %s", op_symbol(e->op),
                                 expr_to_sql(e->kids[0]), tname(e->kids[0]));
            e->dtype = ta;
        }
        break;

    case N_FUNC: {
        const FuncSig *f = NULL;
        for (size_t i = 0; i < sizeof funcs / sizeof funcs[0]; i++)
            if (!strcmp(funcs[i].name, e->s1)) f = &funcs[i];
        for (int i = 0; i < e->nkids; i++) an_expr(e->kids[i], sc, cx);
        if (!f) {
            err(e, "unknown function '%s' (supported: UPPER, LOWER, LENGTH, ABS, ROUND, COALESCE, IFNULL, CONCAT, and aggregates COUNT, SUM, AVG, MIN, MAX)", e->s1);
            e->dtype = DT_UNKNOWN;
            break;
        }
        if (e->nkids < f->min || e->nkids > f->max) {
            err(e, "function %s() expects %d%s argument(s), got %d", f->name, f->min,
                f->max > f->min ? " or more" : "", e->nkids);
        }
        if (f->argcat && e->nkids > 0) {
            int c = tcat(e->kids[0]->dtype);
            if (c && c != f->argcat)
                err(e, "function %s() requires a %s argument, but '%s' is %s", f->name,
                    f->argcat == 1 ? "numeric" : "string", expr_to_sql(e->kids[0]), tname(e->kids[0]));
        }
        e->dtype = f->ret >= 0 ? f->ret : (e->nkids ? e->kids[0]->dtype : DT_UNKNOWN);
        ok("Function %s() is valid", f->name);
        break;
    }

    case N_AGG: {
        if (!(cx.flags & CX_AGG_OK)) {
            err(e, "aggregate function %s() is not allowed in %s", agg_name(e->op), cx.clause);
            e->dtype = DT_UNKNOWN;
            break;
        }
        if (cx.flags & CX_IN_AGG) {
            err(e, "aggregate function calls cannot be nested (%s)", expr_to_sql(e));
            e->dtype = DT_UNKNOWN;
            break;
        }
        if (e->op == AGG_COUNT_STAR) { e->dtype = DT_INT; ok("Aggregate COUNT(*) is valid"); break; }
        Ctx inner = cx;
        inner.flags |= CX_IN_AGG;
        ta = an_expr(e->kids[0], sc, inner);
        int c = tcat(ta);
        switch (e->op) {
        case AGG_COUNT: e->dtype = DT_INT; break;
        case AGG_SUM: case AGG_AVG:
            if (c && c != 1) {
                err(e, "%s() requires a numeric argument, but '%s' is %s", agg_name(e->op),
                    expr_to_sql(e->kids[0]), tname(e->kids[0]));
                e->dtype = DT_UNKNOWN;
                goto agg_done;
            }
            e->dtype = e->op == AGG_AVG ? DT_FLOAT : (dt_is_integer(ta) ? DT_INT : DT_FLOAT);
            break;
        default:
            if (c == 4) err(e, "%s() cannot be applied to a BOOLEAN value", agg_name(e->op));
            e->dtype = ta;
            e->dlen = e->kids[0]->dlen;
        }
        ok("Aggregate %s is valid, returns %s", expr_to_sql(e), dtype_name(e->dtype));
    agg_done:
        break;
    }

    case N_BETWEEN:
        an_expr(e->kids[0], sc, cx);
        an_expr(e->kids[1], sc, cx);
        an_expr(e->kids[2], sc, cx);
        for (int i = 1; i <= 2; i++)
            if (!comparable(e->kids[0], e->kids[i]))
                err(e->kids[i], "type mismatch in BETWEEN: %s vs %s", tname(e->kids[0]), tname(e->kids[i]));
        e->dtype = DT_BOOLEAN;
        break;

    case N_IN_LIST:
        an_expr(e->kids[0], sc, cx);
        for (int i = 0; i < e->kids[1]->nkids; i++) {
            Node *x = e->kids[1]->kids[i];
            an_expr(x, sc, cx);
            if (!comparable(e->kids[0], x))
                err(x, "type mismatch in IN list: %s vs %s ('%s')", tname(e->kids[0]), tname(x), expr_to_sql(x));
        }
        e->dtype = DT_BOOLEAN;
        break;

    case N_IN_SUBQ: {
        an_expr(e->kids[0], sc, cx);
        int st = check_subquery(e, e->kids[1], sc, cx, 1);
        Node tmp = { 0 };
        tmp.type = N_COLREF;
        tmp.dtype = st;
        if (st != DT_UNKNOWN && !comparable(e->kids[0], &tmp))
            err(e, "type mismatch: %s IN (subquery returning %s)", tname(e->kids[0]), dtype_name(st));
        e->dtype = DT_BOOLEAN;
        break;
    }

    case N_LIKE:
        an_expr(e->kids[0], sc, cx);
        an_expr(e->kids[1], sc, cx);
        for (int i = 0; i < 2; i++) {
            int c = tcat(e->kids[i]->dtype);
            if (c && c != 2)
                err(e->kids[i], "LIKE requires string operands, but '%s' is %s", expr_to_sql(e->kids[i]), tname(e->kids[i]));
        }
        e->dtype = DT_BOOLEAN;
        break;

    case N_IS_NULL:
        an_expr(e->kids[0], sc, cx);
        e->dtype = DT_BOOLEAN;
        break;

    case N_SUBQUERY:
        e->dtype = check_subquery(e, e->kids[0], sc, cx, 1);
        break;

    case N_EXISTS:
        check_subquery(e, e->kids[0], sc, cx, 0);
        e->dtype = DT_BOOLEAN;
        break;

    default:
        e->dtype = DT_UNKNOWN;
    }
    return e->dtype;
}

/* ======================================================================
 *  Aggregation checks
 * ====================================================================== */
static void collect_aggs(Node *e, PtrVec *aggs) {
    if (!e) return;
    if (e->type == N_SUBQUERY || e->type == N_EXISTS || e->type == N_IN_SUBQ) {
        if (e->type == N_IN_SUBQ) collect_aggs(e->kids[0], aggs);
        return;
    }
    if (e->type == N_AGG) {
        char *s = canon_sql(e);
        int dup = 0;
        for (int i = 0; i < aggs->n; i++) {
            char *t = canon_sql(aggs->items[i]);
            if (ci_eq(s, t)) dup = 1;
            free(t);
        }
        free(s);
        if (!dup) pv_push(aggs, e);
        return;
    }
    for (int i = 0; i < e->nkids; i++) collect_aggs(e->kids[i], aggs);
}

/* every column used after GROUP BY must be a group key or inside an aggregate */
static void check_grouped(Node *e, QInfo *qi, const char *clause) {
    if (!e) return;
    char *s = canon_sql(e);
    for (int k = 0; k < qi->ngkeys; k++) {
        char *g = canon_sql(qi->gkeys[k]);
        int m = ci_eq(s, g);
        free(g);
        if (m) { e->slot = k; free(s); return; }
    }
    if (e->type == N_AGG) {
        for (int a = 0; a < qi->naggs; a++) {
            char *g = canon_sql(qi->aggs[a]);
            int m = ci_eq(s, g);
            free(g);
            if (m) { e->slot = qi->ngkeys + a; break; }
        }
        free(s);
        return;
    }
    free(s);
    if (e->type == N_COLREF) {
        if (e->depth == 0 && e->dtype != DT_UNKNOWN)
            err(e, "column '%s' in %s must appear in the GROUP BY clause or be used in an aggregate function",
                e->s1 ? xsprintf("%s.%s", e->s1, e->s2) : e->s2, clause);
        return;
    }
    if (e->type == N_SUBQUERY || e->type == N_EXISTS) return;
    if (e->type == N_IN_SUBQ) { check_grouped(e->kids[0], qi, clause); return; }
    for (int i = 0; i < e->nkids; i++) check_grouped(e->kids[i], qi, clause);
}

/* ======================================================================
 *  FROM clause
 * ====================================================================== */
static void add_from(Node *t, Scope *sc) {
    if (!t) return;
    switch (t->type) {
    case N_TABLE_REF: {
        TableSym *ts = lookup_table(t->s1);
        if (ts) {
            ok("Table '%s' found (%d columns)%s%s", ts->name, ts->ncols, t->s2 ? " as alias " : "", t->s2 ? t->s2 : "");
            need_priv(t, ts->name, P_SELECT);
            scope_add(sc, range_from_table(ts, t->s2), t);
            return;
        }
        ViewSym *v = lookup_view(t->s1);
        if (v) {
            Node *vq = sem_view_query(v, 1);
            if (!vq) { err(t, "view '%s' is no longer valid (an object it depends on was changed)", v->name); return; }
            t->info = vq;
            ok("View '%s' found (%d columns) - will be expanded inline", v->name, ((QInfo *)vq->info)->nout);
            need_priv(t, v->name, P_SELECT);
            scope_add(sc, range_from_qinfo(vq->info, t->s2 ? t->s2 : v->name, v->name), t);
            return;
        }
        int n;
        char **names = ht_sorted_keys(g_cat.tables, &n);
        err(t, "table '%s' does not exist%s", t->s1, hint_for(t->s1, names, n));
        free(names);
        return;
    }
    case N_DERIVED_TABLE: {
        if (!t->s2) { err(t, "a subquery in FROM must have an alias, e.g. (SELECT ...) AS t"); return; }
        QInfo *qi = an_query(t->kids[0], sc->parent);
        if (!qi) return;
        for (int i = 0; i < qi->nout; i++)
            for (int j = 0; j < i; j++)
                if (ci_eq(qi->out_names[i], qi->out_names[j])) {
                    err(t, "derived table '%s' has duplicate column name '%s' (use aliases)", t->s2, qi->out_names[i]);
                    return;
                }
        ok("Derived table '%s' is valid (%d columns)", t->s2, qi->nout);
        scope_add(sc, range_from_qinfo(qi, t->s2, t->s2), t);
        return;
    }
    case N_JOIN: {
        add_from(t->kids[0], sc);
        add_from(t->kids[1], sc);
        if (t->kids[2]) {
            Ctx cx = { 0, "JOIN ... ON condition" };
            an_expr(t->kids[2], sc, cx);
            require_bool(t->kids[2], "JOIN condition");
        }
        return;
    }
    default: return;
    }
}

/* ======================================================================
 *  SELECT / set operations / queries
 * ====================================================================== */
static char *item_name(Node *expr, const char *alias) {
    if (alias) return xstrdup(alias);
    if (expr->type == N_COLREF) return xstrdup(expr->s2);
    return expr_to_sql(expr);
}

static void push_item(QInfo *qi, Node *expr, char *name) {
    qi->items = xrealloc(qi->items, sizeof(Node *) * (size_t)(qi->nitems + 1));
    qi->items[qi->nitems++] = expr;
    qi->out_names = xrealloc(qi->out_names, sizeof(char *) * (size_t)qi->nitems);
    qi->out_types = xrealloc(qi->out_types, sizeof(int) * (size_t)qi->nitems);
    qi->out_quals = xrealloc(qi->out_quals, sizeof(char *) * (size_t)qi->nitems);
    qi->out_names[qi->nitems - 1] = name;
    qi->out_types[qi->nitems - 1] = expr->dtype;
    qi->out_quals[qi->nitems - 1] = NULL;
    qi->nout = qi->nitems;
}

static void expand_star(Node *star, Scope *sc, QInfo *qi, int has_from) {
    int found = 0;
    for (int i = 0; i < sc->n; i++) {
        Range *r = sc->r[i];
        if (star->s1 && !ci_eq(star->s1, r->name)) continue;
        found = 1;
        for (int j = 0; j < r->ncols; j++) {
            Node *c = node_new(N_COLREF, star->line, star->col);
            c->s1 = xstrdup(r->name);
            c->s2 = xstrdup(r->cols[j]);
            c->rqual = xstrdup(r->name);
            c->dtype = r->types[j];
            c->dlen = r->lens[j];
            push_item(qi, c, xstrdup(r->cols[j]));
        }
    }
    if (star->s1 && !found) err(star, "unknown table or alias '%s' in '%s.*'", star->s1, star->s1);
    else if (!star->s1 && sc->n == 0) { if (!has_from) err(star, "SELECT * requires a FROM clause"); }
    else ok("'%s' expanded to %d column(s)", star->s1 ? xsprintf("%s.*", star->s1) : "*", qi->nitems);
}

static QInfo *qinfo_new(void) { return xcalloc(1, sizeof(QInfo)); }

static void add_sort(QInfo *q, int col, Node *expr, int desc) {
    q->sort_col = xrealloc(q->sort_col, sizeof(int) * (size_t)(q->nsort + 1));
    q->sort_expr = xrealloc(q->sort_expr, sizeof(Node *) * (size_t)(q->nsort + 1));
    q->sort_desc = xrealloc(q->sort_desc, sizeof(int) * (size_t)(q->nsort + 1));
    q->sort_col[q->nsort] = col;
    q->sort_expr[q->nsort] = expr;
    q->sort_desc[q->nsort] = desc;
    q->nsort++;
    if (col < 0) q->nhidden++;
}

static QInfo *an_select(Node *sel, Node *query, Scope *parent, Node *order, QInfo *qq) {
    QInfo *qi = qinfo_new();
    sel->info = qi;
    Scope *sc = xcalloc(1, sizeof(Scope));
    sc->parent = parent;
    sc->query = query;

    add_from(sel->kids[1], sc);

    if (sel->kids[2]) {
        Ctx cx = { 0, "WHERE clause" };
        an_expr(sel->kids[2], sc, cx);
        require_bool(sel->kids[2], "WHERE clause");
    }
    Node *group = sel->kids[3];
    if (group) {
        Ctx cx = { 0, "GROUP BY clause" };
        for (int i = 0; i < group->nkids; i++) {
            if (group->kids[i]->type == N_INT) {
                err(group->kids[i], "GROUP BY by position is not supported; write the column name");
                continue;
            }
            an_expr(group->kids[i], sc, cx);
        }
        ok("GROUP BY on %d expression(s)", group->nkids);
    }

    Node *items = sel->kids[0];
    Ctx cxs = { CX_AGG_OK, "SELECT list" };
    for (int i = 0; i < items->nkids; i++) {
        Node *it = items->kids[i];
        if (it->type == N_STAR) { expand_star(it, sc, qi, sel->kids[1] != NULL); continue; }
        an_expr(it->kids[0], sc, cxs);
        push_item(qi, it->kids[0], item_name(it->kids[0], it->s1));
        if (it->kids[0]->type == N_COLREF) qi->out_quals[qi->nitems - 1] = xstrdup(it->kids[0]->rqual);
    }

    if (sel->kids[4]) {
        Ctx cx = { CX_AGG_OK, "HAVING clause" };
        an_expr(sel->kids[4], sc, cx);
        require_bool(sel->kids[4], "HAVING clause");
    }

    /* ORDER BY (only when this SELECT is the whole query body) */
    if (order && qq) {
        Ctx cx = { CX_AGG_OK, "ORDER BY clause" };
        for (int i = 0; i < order->nkids; i++) {
            Node *oi = order->kids[i];
            Node *e = oi->kids[0];
            if (e->type == N_INT) {
                if (e->ival < 1 || e->ival > qi->nout)
                    err(e, "ORDER BY position %lld is not in the select list (1..%d)", e->ival, qi->nout);
                else add_sort(qq, (int)e->ival - 1, NULL, oi->flag);
                continue;
            }
            int col = -1;
            if (e->type == N_COLREF && !e->s1) {
                for (int j = 0; j < items->nkids && col < 0; j++)
                    if (items->kids[j]->type == N_SELECT_ITEM && items->kids[j]->s1 && ci_eq(items->kids[j]->s1, e->s2)) {
                        /* position of that item in the expanded list */
                        for (int k = 0; k < qi->nitems; k++)
                            if (qi->items[k] == items->kids[j]->kids[0]) col = k;
                    }
            }
            if (col < 0) {
                an_expr(e, sc, cx);
                char *s = canon_sql(e);
                for (int k = 0; k < qi->nitems && col < 0; k++) {
                    char *t = canon_sql(qi->items[k]);
                    if (ci_eq(s, t)) col = k;
                    free(t);
                }
                free(s);
            }
            if (col >= 0) add_sort(qq, col, NULL, oi->flag);
            else {
                if (sel->flag) err(e, "for SELECT DISTINCT, ORDER BY expressions must appear in the select list");
                add_sort(qq, -1, e, oi->flag);
            }
        }
        ok("ORDER BY on %d key(s)", order->nkids);
    }

    /* aggregation */
    PtrVec aggs = {0};
    for (int i = 0; i < qi->nitems; i++) collect_aggs(qi->items[i], &aggs);
    collect_aggs(sel->kids[4], &aggs);
    if (qq)
        for (int i = 0; i < qq->nsort; i++) if (qq->sort_col[i] < 0) collect_aggs(qq->sort_expr[i], &aggs);
    qi->aggs = (Node **)aggs.items;
    qi->naggs = aggs.n;
    if (group) { qi->gkeys = group->kids; qi->ngkeys = group->nkids; }
    qi->has_agg = qi->naggs > 0 || group || sel->kids[4];
    if (qi->has_agg) {
        for (int i = 0; i < qi->nitems; i++) check_grouped(qi->items[i], qi, "the SELECT list");
        check_grouped(sel->kids[4], qi, "HAVING");
        if (qq)
            for (int i = 0; i < qq->nsort; i++) if (qq->sort_col[i] < 0) check_grouped(qq->sort_expr[i], qi, "ORDER BY");
        if (!group) note("Aggregate query without GROUP BY: the whole input forms a single group");
        else ok("GROUP BY rules satisfied (%d group key(s), %d aggregate(s))", qi->ngkeys, qi->naggs);
    }
    for (int i = 0; i < qi->nitems; i++) qi->out_types[i] = qi->items[i]->dtype;
    return qi;
}

static int types_union(int a, int b, int *res) {
    int ca = tcat(a), cb = tcat(b);
    if (!ca) { *res = b; return 1; }
    if (!cb) { *res = a; return 1; }
    if (ca != cb) return 0;
    if (ca == 1 && (!dt_is_integer(a) || !dt_is_integer(b))) *res = DT_FLOAT;
    else *res = a;
    return 1;
}

static QInfo *an_body(Node *b, Node *q, Scope *parent) {
    if (b->type == N_SELECT) return an_select(b, q, parent, NULL, NULL);
    QInfo *l = an_body(b->kids[0], q, parent);
    QInfo *r = an_body(b->kids[1], q, parent);
    QInfo *qi = qinfo_new();
    b->info = qi;
    if (l->nout != r->nout) {
        err(b, "each %s query must have the same number of columns (%d vs %d)", setop_name(b->op, 0), l->nout, r->nout);
        return qi;
    }
    qi->nout = l->nout;
    qi->out_names = l->out_names;
    qi->out_types = xcalloc((size_t)(l->nout ? l->nout : 1), sizeof(int));
    qi->out_quals = xcalloc((size_t)(l->nout ? l->nout : 1), sizeof(char *));
    for (int i = 0; i < l->nout; i++)
        if (!types_union(l->out_types[i], r->out_types[i], &qi->out_types[i]))
            err(b, "%s column %d has incompatible types %s and %s", setop_name(b->op, b->flag), i + 1,
                dtype_name(l->out_types[i]), dtype_name(r->out_types[i]));
    ok("%s operands are compatible (%d column(s))", setop_name(b->op, b->flag), qi->nout);
    return qi;
}

static QInfo *an_query(Node *q, Scope *parent) {
    if (!q) return NULL;
    QInfo *qi = qinfo_new();
    q->info = qi;
    Node *body = q->kids[0];
    Node *order = node_kid(q, 1);
    QInfo *bi;
    if (body->type == N_SELECT) bi = an_select(body, q, parent, order, qi);
    else {
        bi = an_body(body, q, parent);
        for (int i = 0; order && i < order->nkids; i++) {
            Node *e = order->kids[i]->kids[0];
            int col = -1;
            if (e->type == N_INT && e->ival >= 1 && e->ival <= bi->nout) col = (int)e->ival - 1;
            else if (e->type == N_COLREF && !e->s1)
                for (int k = 0; k < bi->nout && col < 0; k++) if (ci_eq(bi->out_names[k], e->s2)) col = k;
            if (col < 0) err(e, "ORDER BY on a %s must use an output column name or position", setop_name(body->op, 0));
            else add_sort(qi, col, NULL, order->kids[i]->flag);
        }
    }
    qi->nout = bi->nout;
    qi->out_names = bi->out_names;
    qi->out_types = bi->out_types;
    qi->out_quals = bi->out_quals;
    return qi;
}

/* ======================================================================
 *  Views
 * ====================================================================== */
Node *sem_view_query(ViewSym *v, int report) {
    (void)report;
    Node *q = parse_string(v->sql, 0);
    if (!q || q->type != N_QUERY) return NULL;
    int saved_q = quiet_errors;
    quiet++;
    skip_priv++;
    QInfo *qi = an_query(q, NULL);
    skip_priv--;
    quiet--;
    int bad = quiet_errors != saved_q;
    quiet_errors = saved_q;
    if (bad || !qi) return NULL;
    if (v->ncols == 0) {
        v->ncols = qi->nout;
        v->colnames = qi->out_names;
        v->coltypes = qi->out_types;
    }
    return q;
}
int sem_view_columns(ViewSym *v) { return sem_view_query(v, 0) ? v->ncols : -1; }

/* ======================================================================
 *  Assignment compatibility (INSERT / UPDATE / DEFAULT)
 * ====================================================================== */
static void check_assign(ColumnSym *c, Node *e, const char *where) {
    int t = e->dtype;
    char *ct = column_type_str(c);
    if (t == DT_NULL) {
        if (c->not_null) err(e, "column '%s' is NOT NULL, cannot store NULL%s", c->name, where);
        free(ct);
        return;
    }
    if (t == DT_UNKNOWN) { free(ct); return; }
    int cc = tcat(c->type), vc = tcat(t);
    int good = 1;
    if (cc == 1) {
        good = vc == 1;
        if (good && dt_is_integer(c->type) && !dt_is_integer(t))
            warn(e, "FLOAT value %s will be rounded to fit INT column '%s'", expr_to_sql(e), c->name);
    } else if (cc == 2) {
        good = vc == 2;
        if (good && e->type == N_STRING && c->len > 0 && c->type != DT_TEXT && (long long)strlen(e->s1) > c->len)
            err(e, "value '%s' is too long for column '%s' %s (length %zu)%s", e->s1, c->name, ct, strlen(e->s1), where);
    } else if (cc == 3) {
        good = vc == 3 || (vc == 2 && e->type != N_STRING);
        if (!good && e->type == N_STRING) {
            if (string_literal_as(e, c->type)) good = 1;
            else {
                err(e, "'%s' is not a valid %s value for column '%s' (expected %s)%s", e->s1, ct, c->name,
                    c->type == DT_DATE ? "YYYY-MM-DD" : c->type == DT_TIME ? "HH:MM:SS" : "YYYY-MM-DD HH:MM:SS", where);
                free(ct);
                return;
            }
        }
    } else if (cc == 4) {
        good = vc == 4 || (e->type == N_INT && (e->ival == 0 || e->ival == 1));
    }
    if (!good)
        err(e, "type mismatch: column '%s' is %s but the value %s is %s%s", c->name, ct, expr_to_sql(e), tname(e), where);
    free(ct);
}

/* ======================================================================
 *  CREATE TABLE -> TableSym
 * ====================================================================== */
static int dt_compatible_fk(int a, int b) { return tcat(a) == tcat(b); }

static char **names_of(Node *l, int *n) {
    *n = l ? l->nkids : 0;
    char **v = xcalloc((size_t)(*n ? *n : 1), sizeof(char *));
    for (int i = 0; i < *n; i++) v[i] = l->kids[i]->s1;
    return v;
}

static int check_cols_exist(TableSym *t, Node *l, const char *what) {
    int good = 1;
    for (int i = 0; l && i < l->nkids; i++) {
        if (column_index(t, l->kids[i]->s1) < 0) {
            char **nm = xcalloc((size_t)t->ncols + 1, sizeof(char *));
            for (int k = 0; k < t->ncols; k++) nm[k] = t->cols[k].name;
            err(l->kids[i], "column '%s' in %s does not exist in table '%s'%s", l->kids[i]->s1, what, t->name,
                hint_for(l->kids[i]->s1, nm, t->ncols));
            free(nm);
            good = 0;
        }
        for (int j = 0; j < i; j++)
            if (ci_eq(l->kids[i]->s1, l->kids[j]->s1)) { err(l->kids[i], "column '%s' listed twice in %s", l->kids[i]->s1, what); good = 0; }
    }
    return good;
}

/* referenced columns must form the PRIMARY KEY or a UNIQUE key of the parent */
static int is_key_of(TableSym *p, int n, char **cols) {
    for (int k = 0; k < p->nkeys; k++) {
        if (p->keys[k].ncols != n) continue;
        int all = 1;
        for (int i = 0; i < n; i++) {
            int f = 0;
            for (int j = 0; j < n; j++) if (ci_eq(p->keys[k].cols[j], cols[i])) f = 1;
            if (!f) all = 0;
        }
        if (all) return 1;
    }
    return 0;
}

static void add_fk_checked(TableSym *t, Node *at, const char *cname, int n, char **cols, const char *rtable, int rn, char **rcols) {
    if (n != rn) { err(at, "FOREIGN KEY has %d column(s) but REFERENCES lists %d", n, rn); return; }
    TableSym *p = ci_eq(rtable, t->name) ? t : lookup_table(rtable);
    if (!p) { err(at, "referenced table '%s' does not exist", rtable); return; }
    for (int i = 0; i < n; i++) {
        int pc = column_index(p, rcols[i]);
        int cc = column_index(t, cols[i]);
        if (pc < 0) { err(at, "referenced column '%s' does not exist in table '%s'", rcols[i], p->name); return; }
        if (cc < 0) { err(at, "column '%s' does not exist", cols[i]); return; }
        if (!dt_compatible_fk(t->cols[cc].type, p->cols[pc].type)) {
            err(at, "foreign key column '%s' (%s) is not compatible with %s.%s (%s)", cols[i],
                dtype_name(t->cols[cc].type), p->name, rcols[i], dtype_name(p->cols[pc].type));
            return;
        }
    }
    if (!is_key_of(p, n, rcols)) {
        err(at, "referenced column(s) of '%s' must be its PRIMARY KEY or have a UNIQUE constraint", p->name);
        return;
    }
    table_add_fk(t, cname, n, cols, p->name, rcols);
    ok("FOREIGN KEY (%s) -> %s(%s) is valid", cols[0], p->name, rcols[0]);
}

static void add_check_checked(TableSym *t, Node *at, const char *cname, Node *expr) {
    Scope sc = { {0}, 0, NULL, NULL };
    sc.r[sc.n++] = range_from_table(t, NULL);
    Ctx cx = { CX_NO_SUBQ, "CHECK constraints" };
    int before = n_errors + quiet_errors;
    an_expr(expr, &sc, cx);
    if (n_errors + quiet_errors != before) return;
    int c = tcat(expr->dtype);
    if (c && c != 4) { err(at, "CHECK constraint must be a BOOLEAN condition"); return; }
    char *sql = expr_to_sql(expr);
    table_add_check(t, cname, sql, expr);
    ok("CHECK (%s) is valid", sql);
    free(sql);
}

/* process column-level constraints of one column definition */
static void column_constraints(TableSym *t, Node *cd, int *pk_seen, int allow_pk) {
    ColumnSym *c = &t->cols[column_index(t, cd->s1)];
    Node *cons = cd->kids[1];
    int seen_null = 0, seen_notnull = 0, seen_default = 0;
    for (int i = 0; cons && i < cons->nkids; i++) {
        Node *k = cons->kids[i];
        char *cn[1] = { c->name };
        switch (k->op) {
        case C_PRIMARY:
            if (!allow_pk) { err(k, "cannot add a PRIMARY KEY column with ALTER TABLE"); break; }
            if (*pk_seen) { err(k, "multiple primary keys for table '%s' are not allowed", t->name); break; }
            *pk_seen = 1;
            table_add_key(t, C_PRIMARY, k->s3, 1, cn);
            ok("PRIMARY KEY on '%s' (implies NOT NULL + UNIQUE)", c->name);
            break;
        case C_NOTNULL: seen_notnull = 1; c->not_null = 1; break;
        case C_NULL: seen_null = 1; break;
        case C_UNIQUE:
            table_add_key(t, C_UNIQUE, k->s3, 1, cn);
            ok("UNIQUE constraint on '%s'", c->name);
            break;
        case C_DEFAULT: {
            if (seen_default++) { err(k, "multiple DEFAULT values for column '%s'", c->name); break; }
            Scope sc = { {0}, 0, NULL, NULL };
            Ctx cx = { CX_NO_SUBQ, "DEFAULT values" };
            an_expr(k->kids[0], &sc, cx);
            check_assign(c, k->kids[0], " (DEFAULT value)");
            c->default_sql = expr_to_sql(k->kids[0]);
            c->default_expr = k->kids[0];
            ok("DEFAULT %s is compatible with %s", c->default_sql, dtype_name(c->type));
            break;
        }
        case C_CHECK:
            add_check_checked(t, k, k->s3, k->kids[0]);
            break;
        case C_FOREIGN: {
            char *rc[1] = { k->s2 };
            add_fk_checked(t, k, k->s3, 1, cn, k->s1, 1, rc);
            break;
        }
        }
    }
    if (seen_null && seen_notnull) err(cd, "conflicting NULL / NOT NULL declarations for column '%s'", c->name);
    if (seen_null && c->primary) err(cd, "PRIMARY KEY column '%s' cannot be declared NULL", c->name);
    if (c->not_null && c->default_expr && c->default_expr->type == N_NULL)
        err(cd, "column '%s' is NOT NULL but its DEFAULT is NULL", c->name);
}

static int column_def_basic(TableSym *t, Node *cd) {
    if (t->ncols >= 250) { err(cd, "a table can have at most 250 columns"); return 0; }
    if (column_index(t, cd->s1) >= 0) { err(cd, "column '%s' is specified more than once", cd->s1); return 0; }
    Node *dt = cd->kids[0];
    if ((dt->op == DT_VARCHAR || dt->op == DT_CHAR) && dt->ival <= 0) {
        err(dt, "%s length must be greater than zero", dtype_name(dt->op));
        return 0;
    }
    if (dt->op == DT_DECIMAL || dt->op == DT_NUMERIC) {
        if (dt->ival > 38) err(dt, "DECIMAL precision %lld exceeds the maximum of 38", dt->ival);
        if (dt->flag > dt->ival && dt->ival > 0) err(dt, "DECIMAL scale %d cannot exceed precision %lld", dt->flag, dt->ival);
    }
    insert_column(t, cd->s1, dt->op, dt->ival, dt->flag);
    return 1;
}

TableSym *sem_build_table(Node *ct, int report) {
    if (!report) quiet++;
    int before = n_errors + quiet_errors;
    TableSym *t = table_new(ct->s1);
    int pk_seen = 0;
    for (int i = 0; i < ct->nkids; i++)
        if (ct->kids[i]->type == N_COLUMN_DEF) column_def_basic(t, ct->kids[i]);
    if (t->ncols == 0) err(ct, "a table must have at least one column");
    else ok("%d column(s) declared, no duplicates", t->ncols);
    for (int i = 0; i < ct->nkids; i++)
        if (ct->kids[i]->type == N_TABLE_CONSTRAINT && ct->kids[i]->op == C_PRIMARY) pk_seen++;
    if (pk_seen > 1) err(ct, "multiple primary keys for table '%s' are not allowed", t->name);
    int col_pk = 0;
    for (int i = 0; i < ct->nkids; i++)
        if (ct->kids[i]->type == N_COLUMN_DEF && column_index(t, ct->kids[i]->s1) >= 0) {
            int dummy = pk_seen || col_pk;
            int before_pk = dummy;
            column_constraints(t, ct->kids[i], &dummy, 1);
            if (dummy && !before_pk) col_pk = 1;
        }
    for (int i = 0; i < ct->nkids; i++) {
        Node *k = ct->kids[i];
        if (k->type != N_TABLE_CONSTRAINT) continue;
        int n;
        char **cols = names_of(k->kids[0], &n);
        switch (k->op) {
        case C_PRIMARY:
            if (col_pk) { err(k, "multiple primary keys for table '%s' are not allowed", t->name); break; }
            if (check_cols_exist(t, k->kids[0], "PRIMARY KEY")) {
                table_add_key(t, C_PRIMARY, k->s3, n, cols);
                ok("PRIMARY KEY (%s%s) is valid", cols[0], n > 1 ? ", ..." : "");
            }
            break;
        case C_UNIQUE:
            if (check_cols_exist(t, k->kids[0], "UNIQUE")) {
                table_add_key(t, C_UNIQUE, k->s3, n, cols);
                ok("UNIQUE (%s%s) is valid", cols[0], n > 1 ? ", ..." : "");
            }
            break;
        case C_FOREIGN: {
            if (!check_cols_exist(t, k->kids[0], "FOREIGN KEY")) break;
            int rn;
            char **rcols = names_of(k->kids[1], &rn);
            add_fk_checked(t, k, k->s3, n, cols, k->s1, rn, rcols);
            break;
        }
        case C_CHECK:
            add_check_checked(t, k, k->s3, k->kids[2]);
            break;
        }
    }
    if (!report) quiet--;
    int bad = (n_errors + quiet_errors) != before;
    if (!report) quiet_errors = 0;
    return bad ? NULL : t;
}

/* ======================================================================
 *  Statements
 * ====================================================================== */
static int reserved_name(Node *at, const char *name) {
    if (!strncasecmp(name, "__sqlc", 6)) {
        err(at, "names starting with '__sqlc' are reserved for the compiler's catalog");
        return 1;
    }
    return 0;
}

static void an_create_table(Node *s) {
    if (!need_admin(s, "CREATE TABLE")) return;
    if (reserved_name(s, s->s1)) return;
    if (lookup_table(s->s1)) {
        if (s->flag) { note("Table '%s' already exists - IF NOT EXISTS: nothing to do", s->s1); return; }
        err(s, "table '%s' already exists", s->s1);
        return;
    }
    if (ht_get(g_cat.views, s->s1)) { err(s, "a view named '%s' already exists", s->s1); return; }
    ok("Table name '%s' is available", s->s1);
    s->info = sem_build_table(s, 1);
}

/* constant rows of INSERT ... VALUES: evaluate CHECK constraints and keys now */
static void insert_constant_checks(TableSym *t, Node *s, int *map, int nvals) {
    Node *rows = s->kids[1];
    int pk_idx[16], npk = 0;
    KeySym *pk = table_primary_key(t);
    if (pk) for (int i = 0; i < pk->ncols && i < 16; i++) pk_idx[npk++] = column_index(t, pk->cols[i]);
    Value **built = xcalloc((size_t)rows->nkids, sizeof(Value *));
    for (int r = 0; r < rows->nkids; r++) {
        Node *row = rows->kids[r];
        int constant = 1;
        for (int i = 0; i < row->nkids; i++) {
            int ty = row->kids[i]->type;
            if (!(ty == N_INT || ty == N_FLOAT || ty == N_STRING || ty == N_DATE || ty == N_NULL || ty == N_BOOL ||
                  (ty == N_UNOP && row->kids[i]->kids[0]->type != N_COLREF))) constant = 0;
        }
        if (!constant) continue;
        Relation *rel = rel_new(t->ncols);
        for (int c = 0; c < t->ncols; c++) rel_set_col(rel, c, t->name, t->cols[c].name, t->cols[c].type, t->cols[c].len);
        Value *vals = row_new(t->ncols);
        jmp_buf jb;
        jmp_buf *saved = g_exec_jmp;
        g_exec_jmp = &jb;
        if (setjmp(jb)) { g_exec_jmp = saved; continue; }
        int good = 1;
        for (int c = 0; c < t->ncols; c++) {
            int vi = -1;
            for (int k = 0; k < nvals; k++) if (map[k] == c) vi = k;
            Value v = vi >= 0 ? eval_expr(row->kids[vi], NULL) : (t->cols[c].default_expr ? eval_expr(t->cols[c].default_expr, NULL) : v_null());
            char e[256];
            if (!coerce_value(v, t->cols[c].type, t->cols[c].len, t->cols[c].scale, &vals[c], e, sizeof e)) {
                err(vi >= 0 ? row->kids[vi] : row, "row %d, column '%s': %s", r + 1, t->cols[c].name, e);
                good = 0;
            }
        }
        g_exec_jmp = saved;
        if (!good) continue;
        Env env = { rel, vals, 0, NULL };
        for (volatile int k = 0; k < t->nchecks; k++) {
            g_exec_jmp = &jb;
            if (setjmp(jb)) { g_exec_jmp = saved; continue; }
            Value cv = eval_expr(t->checks[k].expr, &env);
            g_exec_jmp = saved;
            if (v_truth(cv) == 0)
                err(row, "row %d violates CHECK constraint (%s)", r + 1, t->checks[k].sql);
        }
        /* duplicate primary key inside this INSERT */
        if (npk) {
            built[r] = vals;
            for (int p = 0; p < r; p++) {
                if (!built[p]) continue;
                int same = 1;
                for (int k = 0; k < npk; k++) if (!v_same(built[p][pk_idx[k]], vals[pk_idx[k]])) same = 0;
                if (same) {
                    char *kv = v_to_text(vals[pk_idx[0]], 0);
                    err(row, "rows %d and %d have the same PRIMARY KEY value (%s = %s)", p + 1, r + 1, pk->cols[0], kv);
                    free(kv);
                }
            }
        }
    }
    free(built);
}

static void an_insert(Node *s) {
    TableSym *t = lookup_table(s->s1);
    if (!t) {
        if (ht_get(g_cat.views, s->s1)) err(s, "cannot INSERT into view '%s' (views are read-only)", s->s1);
        else {
            int n;
            char **names = ht_sorted_keys(g_cat.tables, &n);
            err(s, "table '%s' does not exist%s", s->s1, hint_for(s->s1, names, n));
            free(names);
        }
        return;
    }
    ok("Table '%s' found (%d columns)", t->name, t->ncols);
    need_priv(s, t->name, P_INSERT);
    int map[256], nvals;
    Node *cl = s->kids[0];
    if (cl) {
        if (!check_cols_exist(t, cl, "INSERT column list")) return;
        nvals = cl->nkids;
        for (int i = 0; i < nvals; i++) map[i] = column_index(t, cl->kids[i]->s1);
        ok("Column list valid (%d of %d columns)", nvals, t->ncols);
    } else {
        nvals = t->ncols;
        for (int i = 0; i < nvals; i++) map[i] = i;
    }
    /* columns not supplied need a DEFAULT or must be nullable */
    for (int c = 0; c < t->ncols; c++) {
        int given = 0;
        for (int i = 0; i < nvals; i++) if (map[i] == c) given = 1;
        if (!given && t->cols[c].not_null && !t->cols[c].default_expr)
            err(s, "column '%s' is NOT NULL and has no DEFAULT, so it must be given a value", t->cols[c].name);
        else if (!given) note("Column '%s' not supplied -> %s", t->cols[c].name,
                              t->cols[c].default_sql ? xsprintf("DEFAULT %s", t->cols[c].default_sql) : "NULL");
    }
    Node *src = s->kids[1];
    if (src->type == N_QUERY) {
        QInfo *qi = an_query(src, NULL);
        if (!qi) return;
        if (qi->nout != nvals) { err(src, "INSERT expects %d column(s) but the SELECT returns %d", nvals, qi->nout); return; }
        for (int i = 0; i < nvals; i++) {
            Node tmp = { 0 };
            tmp.type = N_COLREF;
            tmp.dtype = qi->out_types[i];
            tmp.line = src->line; tmp.col = src->col;
            check_assign(&t->cols[map[i]], &tmp, "");
        }
        ok("SELECT result is compatible with the target columns");
        return;
    }
    Scope sc = { {0}, 0, NULL, NULL };
    Ctx cx = { 0, "VALUES" };
    int before = n_errors;
    for (int r = 0; r < src->nkids; r++) {
        Node *row = src->kids[r];
        if (row->nkids != nvals) {
            err(row, "row %d has %d value(s) but %d column(s) are expected", r + 1, row->nkids, nvals);
            continue;
        }
        char where[32];
        snprintf(where, sizeof where, " (row %d)", r + 1);
        for (int i = 0; i < nvals; i++) {
            an_expr(row->kids[i], &sc, cx);
            check_assign(&t->cols[map[i]], row->kids[i], src->nkids > 1 ? where : "");
        }
    }
    if (n_errors == before) {
        ok("%d row(s) type-checked against the table schema", src->nkids);
        insert_constant_checks(t, s, map, nvals);
        if (n_errors == before && t->nchecks) ok("CHECK constraints satisfied by the constant values");
    }
}

static void an_update(Node *s) {
    TableSym *t = lookup_table(s->s1);
    if (!t) {
        if (ht_get(g_cat.views, s->s1)) err(s, "cannot UPDATE view '%s' (views are read-only)", s->s1);
        else err(s, "table '%s' does not exist", s->s1);
        return;
    }
    ok("Table '%s' found (%d columns)", t->name, t->ncols);
    need_priv(s, t->name, P_UPDATE);
    Scope sc = { {0}, 0, NULL, NULL };
    sc.r[sc.n++] = range_from_table(t, NULL);
    Ctx cx = { 0, "UPDATE ... SET" };
    Node *al = s->kids[0];
    for (int i = 0; i < al->nkids; i++) {
        Node *a = al->kids[i];
        ColumnSym *c = lookup_column(t, a->s1);
        if (!c) {
            char **nm = xcalloc((size_t)t->ncols + 1, sizeof(char *));
            for (int k = 0; k < t->ncols; k++) nm[k] = t->cols[k].name;
            err(a, "column '%s' does not exist in table '%s'%s", a->s1, t->name, hint_for(a->s1, nm, t->ncols));
            free(nm);
            continue;
        }
        for (int j = 0; j < i; j++) if (ci_eq(al->kids[j]->s1, a->s1)) err(a, "column '%s' is assigned more than once", a->s1);
        an_expr(a->kids[0], &sc, cx);
        check_assign(c, a->kids[0], "");
        ok("SET %s = %s  (%s <- %s)", c->name, expr_to_sql(a->kids[0]), dtype_name(c->type), tname(a->kids[0]));
    }
    if (s->kids[1]) {
        Ctx wx = { 0, "WHERE clause" };
        an_expr(s->kids[1], &sc, wx);
        require_bool(s->kids[1], "WHERE clause");
    } else warn(s, "UPDATE without WHERE will modify every row of '%s'", t->name);
}

static void an_delete(Node *s) {
    TableSym *t = lookup_table(s->s1);
    if (!t) {
        if (ht_get(g_cat.views, s->s1)) err(s, "cannot DELETE from view '%s' (views are read-only)", s->s1);
        else err(s, "table '%s' does not exist", s->s1);
        return;
    }
    ok("Table '%s' found", t->name);
    need_priv(s, t->name, P_DELETE);
    if (s->kids[0]) {
        Scope sc = { {0}, 0, NULL, NULL };
        sc.r[sc.n++] = range_from_table(t, NULL);
        Ctx cx = { 0, "WHERE clause" };
        an_expr(s->kids[0], &sc, cx);
        require_bool(s->kids[0], "WHERE clause");
    } else note("DELETE without WHERE removes every row of '%s'", t->name);
}

static const char *referencing_table(const char *name, const char *except_col) {
    int n;
    char **keys = ht_sorted_keys(g_cat.tables, &n);
    const char *res = NULL;
    for (int i = 0; i < n && !res; i++) {
        TableSym *o = ht_get(g_cat.tables, keys[i]);
        if (ci_eq(o->name, name)) continue;
        for (int f = 0; f < o->nfks; f++)
            if (ci_eq(o->fks[f].ref_table, name)) {
                if (except_col) {
                    for (int c = 0; c < o->fks[f].ncols; c++)
                        if (ci_eq(o->fks[f].ref_cols[c], except_col)) res = o->name;
                } else res = o->name;
            }
    }
    free(keys);
    return res;
}

static int expr_uses_column(Node *e, const char *col) {
    if (!e) return 0;
    if (e->type == N_COLREF && ci_eq(e->s2, col)) return 1;
    for (int i = 0; i < e->nkids; i++) if (expr_uses_column(e->kids[i], col)) return 1;
    return 0;
}

static void an_alter(Node *s) {
    if (!need_admin(s, "ALTER TABLE")) return;
    TableSym *t = lookup_table(s->s1);
    if (!t) { err(s, "table '%s' does not exist", s->s1); return; }
    ok("Table '%s' found", t->name);
    Node *a = s->kids[0];
    TableSym *nt = table_copy(t);
    s->info = nt;
    int ci;
    switch (a->op) {
    case A_ADD: {
        Node *cd = a->kids[0];
        if (column_index(nt, cd->s1) >= 0) { err(cd, "column '%s' already exists in table '%s'", cd->s1, t->name); return; }
        if (!column_def_basic(nt, cd)) return;
        int pk = 0;
        column_constraints(nt, cd, &pk, 0);
        ok("Column '%s' can be added", cd->s1);
        break;
    }
    case A_DROP: {
        ci = column_index(t, a->s1);
        if (ci < 0) { err(a, "column '%s' does not exist in table '%s'", a->s1, t->name); return; }
        if (t->ncols == 1) { err(a, "cannot drop the only column of table '%s'", t->name); return; }
        if (t->cols[ci].primary) { err(a, "cannot drop column '%s': it is part of the PRIMARY KEY", a->s1); return; }
        for (int k = 0; k < t->nkeys; k++)
            if (t->keys[k].ncols > 1)
                for (int j = 0; j < t->keys[k].ncols; j++)
                    if (ci_eq(t->keys[k].cols[j], a->s1)) { err(a, "cannot drop column '%s': it is part of a multi-column UNIQUE key", a->s1); return; }
        for (int k = 0; k < t->nchecks; k++) {
            if (expr_uses_column(t->checks[k].expr, a->s1)) {
                err(a, "cannot drop column '%s': it is used by CHECK (%s)", a->s1, t->checks[k].sql);
                return;
            }
        }
        const char *rt = referencing_table(t->name, a->s1);
        if (rt) { err(a, "cannot drop column '%s': it is referenced by a FOREIGN KEY in table '%s'", a->s1, rt); return; }
        int n;
        char **keys = ht_sorted_keys(g_cat.indexes, &n);
        for (int i = 0; i < n; i++) {
            IndexSym *ix = ht_get(g_cat.indexes, keys[i]);
            if (!ci_eq(ix->table, t->name)) continue;
            for (int j = 0; j < ix->ncols; j++)
                if (ci_eq(ix->cols[j], a->s1)) { err(a, "cannot drop column '%s': it is used by index '%s' (drop the index first)", a->s1, ix->name); free(keys); return; }
        }
        free(keys);
        /* build new table symbol without the column */
        TableSym *x = table_new(t->name);
        for (int i = 0; i < t->ncols; i++) {
            if (i == ci) continue;
            ColumnSym *c = insert_column(x, t->cols[i].name, t->cols[i].type, t->cols[i].len, t->cols[i].scale);
            *c = t->cols[i];
        }
        for (int k = 0; k < t->nkeys; k++)
            if (!(t->keys[k].ncols == 1 && ci_eq(t->keys[k].cols[0], a->s1)))
                table_add_key(x, t->keys[k].kind, t->keys[k].name, t->keys[k].ncols, t->keys[k].cols);
        for (int k = 0; k < t->nfks; k++) {
            int uses = 0;
            for (int j = 0; j < t->fks[k].ncols; j++) if (ci_eq(t->fks[k].cols[j], a->s1)) uses = 1;
            if (uses) note("FOREIGN KEY on '%s' is dropped together with the column", a->s1);
            else table_add_fk(x, t->fks[k].name, t->fks[k].ncols, t->fks[k].cols, t->fks[k].ref_table, t->fks[k].ref_cols);
        }
        for (int k = 0; k < t->nchecks; k++) table_add_check(x, t->checks[k].name, t->checks[k].sql, t->checks[k].expr);
        s->info = x;
        ok("Column '%s' can be dropped", a->s1);
        break;
    }
    case A_MODIFY: {
        Node *cd = a->kids[0];
        ci = column_index(nt, cd->s1);
        if (ci < 0) { err(cd, "column '%s' does not exist in table '%s'", cd->s1, t->name); return; }
        ColumnSym *c = &nt->cols[ci];
        Node *dt = cd->kids[0];
        if ((dt->op == DT_VARCHAR || dt->op == DT_CHAR) && dt->ival <= 0) { err(dt, "length must be greater than zero"); return; }
        int oldcat = tcat(c->type), newcat = tcat(dt->op);
        c->type = dt->op; c->len = dt->ival; c->scale = dt->flag;
        if (!c->primary) c->not_null = 0;
        c->default_sql = NULL; c->default_expr = NULL;
        int pk = 1;   /* PRIMARY KEY not allowed in MODIFY unless already */
        Node *cons = cd->kids[1];
        for (int i = 0; cons && i < cons->nkids; i++)
            if (cons->kids[i]->op == C_PRIMARY && !c->primary) err(cons->kids[i], "use a new table to change the PRIMARY KEY");
        Node filtered = *cd;
        Node *fl = mk_list();
        for (int i = 0; cons && i < cons->nkids; i++) if (cons->kids[i]->op != C_PRIMARY) node_add(fl, cons->kids[i]);
        filtered.kids = xmalloc(sizeof(Node *) * 2);
        filtered.kids[0] = dt; filtered.kids[1] = fl; filtered.nkids = 2;
        column_constraints(nt, &filtered, &pk, 0);
        /* type change must remain compatible with foreign keys */
        for (int k = 0; k < nt->nfks; k++)
            for (int j = 0; j < nt->fks[k].ncols; j++)
                if (ci_eq(nt->fks[k].cols[j], c->name)) {
                    TableSym *p = lookup_table(nt->fks[k].ref_table);
                    int pc = p ? column_index(p, nt->fks[k].ref_cols[j]) : -1;
                    if (pc >= 0 && tcat(p->cols[pc].type) != newcat)
                        err(dt, "new type %s is incompatible with the FOREIGN KEY to %s.%s", dtype_name(dt->op), p->name, p->cols[pc].name);
                }
        if (referencing_table(t->name, c->name) && oldcat != newcat)
            err(dt, "column '%s' is referenced by a FOREIGN KEY; its type category cannot change", c->name);
        if (oldcat != newcat) note("Existing values will be converted from %s to %s during execution", dtype_name(t->cols[ci].type), dtype_name(dt->op));
        ok("Column '%s' can be modified to %s", c->name, dtype_name(c->type));
        break;
    }
    case A_RENAME_COL: {
        ci = column_index(nt, a->s1);
        if (ci < 0) { err(a, "column '%s' does not exist in table '%s'", a->s1, t->name); return; }
        if (column_index(nt, a->s2) >= 0) { err(a, "column '%s' already exists in table '%s'", a->s2, t->name); return; }
        snprintf(nt->cols[ci].name, MAX_NAME, "%s", a->s2);
        for (int k = 0; k < nt->nkeys; k++)
            for (int j = 0; j < nt->keys[k].ncols; j++)
                if (ci_eq(nt->keys[k].cols[j], a->s1)) nt->keys[k].cols[j] = xstrdup(a->s2);
        for (int k = 0; k < nt->nfks; k++)
            for (int j = 0; j < nt->fks[k].ncols; j++) {
                if (ci_eq(nt->fks[k].cols[j], a->s1)) nt->fks[k].cols[j] = xstrdup(a->s2);
                if (ci_eq(nt->fks[k].ref_table, t->name) && ci_eq(nt->fks[k].ref_cols[j], a->s1)) nt->fks[k].ref_cols[j] = xstrdup(a->s2);
            }
        ok("Column '%s' can be renamed to '%s'", a->s1, a->s2);
        break;
    }
    case A_RENAME_TABLE:
        if (ht_get(g_cat.tables, a->s2) || ht_get(g_cat.views, a->s2)) { err(a, "a table or view named '%s' already exists", a->s2); return; }
        if (reserved_name(a, a->s2)) return;
        snprintf(nt->name, MAX_NAME, "%s", a->s2);
        for (int k = 0; k < nt->nfks; k++) if (ci_eq(nt->fks[k].ref_table, t->name)) nt->fks[k].ref_table = xstrdup(a->s2);
        ok("Table '%s' can be renamed to '%s'", t->name, a->s2);
        break;
    }
}

static void an_statement(Node *s);

static void an_statement(Node *s) {
    switch (s->type) {
    case N_QUERY: {
        QInfo *qi = an_query(s, NULL);
        if (qi && !quiet) ok("Query returns %d column(s)", qi->nout);
        break;
    }
    case N_INSERT: an_insert(s); break;
    case N_UPDATE: an_update(s); break;
    case N_DELETE: an_delete(s); break;
    case N_CREATE_TABLE: an_create_table(s); break;
    case N_ALTER_TABLE: an_alter(s); break;

    case N_CREATE_VIEW: {
        if (!need_admin(s, "CREATE VIEW")) return;
        if (reserved_name(s, s->s1)) return;
        if (ht_get(g_cat.tables, s->s1) || ht_get(g_cat.views, s->s1)) { err(s, "a table or view named '%s' already exists", s->s1); return; }
        QInfo *qi = an_query(s->kids[0], NULL);
        if (!qi) return;
        for (int i = 0; i < qi->nout; i++)
            for (int j = 0; j < i; j++)
                if (ci_eq(qi->out_names[i], qi->out_names[j]))
                    err(s, "view '%s' would have two columns named '%s' (add an alias with AS)", s->s1, qi->out_names[i]);
        ok("View '%s' defines %d column(s)", s->s1, qi->nout);
        break;
    }
    case N_CREATE_INDEX: {
        if (!need_admin(s, "CREATE INDEX")) return;
        if (reserved_name(s, s->s1)) return;
        if (ht_get(g_cat.indexes, s->s1)) { err(s, "index '%s' already exists", s->s1); return; }
        TableSym *t = lookup_table(s->s2);
        if (!t) { err(s, "table '%s' does not exist", s->s2); return; }
        if (check_cols_exist(t, s->kids[0], "CREATE INDEX")) ok("Index columns exist in '%s'", t->name);
        break;
    }
    case N_DROP_TABLE: {
        if (!need_admin(s, "DROP TABLE")) return;
        TableSym *t = lookup_table(s->s1);
        if (!t) {
            if (ht_get(g_cat.views, s->s1)) err(s, "'%s' is a view; use DROP VIEW", s->s1);
            else if (s->flag) note("Table '%s' does not exist - IF EXISTS: nothing to do", s->s1);
            else err(s, "table '%s' does not exist", s->s1);
            return;
        }
        const char *rt = referencing_table(t->name, NULL);
        if (rt) { err(s, "cannot drop table '%s': table '%s' has a FOREIGN KEY referencing it", t->name, rt); return; }
        ok("Table '%s' can be dropped (no foreign keys reference it)", t->name);
        break;
    }
    case N_DROP_VIEW:
        if (!need_admin(s, "DROP VIEW")) return;
        if (!lookup_view(s->s1)) {
            if (ht_get(g_cat.tables, s->s1)) err(s, "'%s' is a table; use DROP TABLE", s->s1);
            else if (s->flag) note("View '%s' does not exist - IF EXISTS: nothing to do", s->s1);
            else err(s, "view '%s' does not exist", s->s1);
        } else ok("View '%s' found", s->s1);
        break;
    case N_DROP_INDEX:
        if (!need_admin(s, "DROP INDEX")) return;
        if (!lookup_index(s->s1)) {
            if (s->flag) note("Index '%s' does not exist - IF EXISTS: nothing to do", s->s1);
            else err(s, "index '%s' does not exist", s->s1);
        } else ok("Index '%s' found", s->s1);
        break;
    case N_TRUNCATE: {
        TableSym *t = lookup_table(s->s1);
        if (!t) { err(s, "table '%s' does not exist", s->s1); return; }
        need_priv(s, t->name, P_DELETE);
        const char *rt = referencing_table(t->name, NULL);
        if (rt) note("Table '%s' is referenced by '%s'; TRUNCATE fails if referencing rows exist", t->name, rt);
        ok("Table '%s' can be truncated", t->name);
        break;
    }
    case N_CREATE_DATABASE:
        if (!need_admin(s, "CREATE DATABASE")) return;
        if (txn_active()) { err(s, "CREATE DATABASE cannot run inside a transaction"); return; }
        if (db_file_exists(s->s1)) {
            if (s->flag) note("Database '%s' already exists - IF NOT EXISTS: nothing to do", s->s1);
            else err(s, "database '%s' already exists", s->s1);
        } else ok("Database name '%s' is available", s->s1);
        break;
    case N_DROP_DATABASE:
        if (!need_admin(s, "DROP DATABASE")) return;
        if (txn_active()) { err(s, "DROP DATABASE cannot run inside a transaction"); return; }
        if (!db_file_exists(s->s1)) {
            if (s->flag) note("Database '%s' does not exist - IF EXISTS: nothing to do", s->s1);
            else err(s, "database '%s' does not exist", s->s1);
        } else if (ci_eq(s->s1, g_dbname)) err(s, "cannot drop the database currently in use ('%s'); USE another one first", s->s1);
        else ok("Database '%s' can be dropped", s->s1);
        break;
    case N_USE:
        if (txn_active()) { err(s, "cannot switch database inside a transaction (COMMIT or ROLLBACK first)"); return; }
        if (!db_file_exists(s->s1)) err(s, "database '%s' does not exist (CREATE DATABASE %s first)", s->s1, s->s1);
        else ok("Database '%s' exists", s->s1);
        break;

    case N_GRANT: case N_REVOKE: {
        const char *what = s->type == N_GRANT ? "GRANT" : "REVOKE";
        if (!need_admin(s, what)) return;
        if (!lookup_table(s->s1) && !lookup_view(s->s1)) { err(s, "table or view '%s' does not exist", s->s1); return; }
        ok("Object '%s' exists", s->s1);
        for (int i = 0; i < s->kids[1]->nkids; i++) {
            Node *u = s->kids[1]->kids[i];
            UserSym *us = lookup_user(u->s1);
            if (!us) {
                if (s->type == N_GRANT) note("User '%s' does not exist - it will be created", u->s1);
                else err(u, "user '%s' does not exist", u->s1);
            } else if (us->is_admin) warn(u, "'%s' is an administrator and already has every privilege", u->s1);
            else ok("User '%s' exists", u->s1);
        }
        break;
    }
    case N_CREATE_USER:
        if (!need_admin(s, "CREATE USER")) return;
        if (lookup_user(s->s1)) err(s, "user '%s' already exists", s->s1);
        else ok("User name '%s' is available", s->s1);
        break;
    case N_DROP_USER: {
        if (!need_admin(s, "DROP USER")) return;
        UserSym *u = lookup_user(s->s1);
        if (!u) err(s, "user '%s' does not exist", s->s1);
        else if (u->is_admin) err(s, "the administrator account cannot be dropped");
        else if (ci_eq(u->name, g_current_user)) err(s, "cannot drop the user you are connected as");
        else ok("User '%s' can be dropped", s->s1);
        break;
    }
    case N_CONNECT:
        if (!lookup_user(s->s1)) err(s, "user '%s' does not exist (CREATE USER %s first)", s->s1, s->s1);
        else ok("User '%s' exists", s->s1);
        break;

    case N_BEGIN:
        if (txn_active()) err(s, "a transaction is already active (id %d); COMMIT or ROLLBACK it first", g_txn.id);
        else ok("No transaction active - a new one can start");
        break;
    case N_COMMIT: case N_ROLLBACK:
        if (!txn_active()) warn(s, "there is no active transaction (autocommit mode) - nothing to %s", s->type == N_COMMIT ? "commit" : "roll back");
        else ok("Transaction %d is active", g_txn.id);
        break;
    case N_SAVEPOINT:
        if (!txn_active()) note("No active transaction - SAVEPOINT will start one implicitly");
        else ok("Transaction %d is active", g_txn.id);
        break;
    case N_ROLLBACK_TO: case N_RELEASE:
        if (!txn_active()) err(s, "there is no active transaction, so savepoint '%s' does not exist", s->s1);
        else if (!txn_has_savepoint(s->s1)) err(s, "savepoint '%s' does not exist in transaction %d", s->s1, g_txn.id);
        else ok("Savepoint '%s' found", s->s1);
        break;

    case N_SHOW:
        if (s->op == SH_GRANTS && s->s1 && !lookup_user(s->s1)) err(s, "user '%s' does not exist", s->s1);
        else ok("SHOW statement is valid");
        break;
    case N_DESCRIBE:
        if (!lookup_table(s->s1) && !lookup_view(s->s1)) err(s, "table or view '%s' does not exist", s->s1);
        else ok("Object '%s' found", s->s1);
        break;
    case N_EXPLAIN:
        an_statement(s->kids[0]);
        break;
    default: break;
    }
}

int sem_analyze(Node *stmt) {
    n_errors = 0;
    quiet = 0;
    quiet_errors = 0;
    skip_priv = 0;
    for (int i = 0; i < logged.n; i++) free(logged.items[i]);
    logged.n = 0;
    an_statement(stmt);
    return n_errors;
}
