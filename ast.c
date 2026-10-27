/*
 * ast.c - AST construction, pretty printing and un-parsing (AST -> SQL text)
 */
#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_unparse_subq_ref = 0;
int g_unparse_resolved = 0;

/* ======================================================================
 *  Data type helpers
 * ====================================================================== */
const char *dtype_name(int dt) {
    switch (dt) {
    case DT_NULL: return "NULL";
    case DT_INT: return "INT";
    case DT_SMALLINT: return "SMALLINT";
    case DT_BIGINT: return "BIGINT";
    case DT_FLOAT: return "FLOAT";
    case DT_DOUBLE: return "DOUBLE";
    case DT_DECIMAL: return "DECIMAL";
    case DT_NUMERIC: return "NUMERIC";
    case DT_CHAR: return "CHAR";
    case DT_VARCHAR: return "VARCHAR";
    case DT_TEXT: return "TEXT";
    case DT_DATE: return "DATE";
    case DT_TIME: return "TIME";
    case DT_DATETIME: return "DATETIME";
    case DT_BOOLEAN: return "BOOLEAN";
    default: return "UNKNOWN";
    }
}
int dt_is_integer(int dt) { return dt == DT_INT || dt == DT_SMALLINT || dt == DT_BIGINT; }
int dt_is_numeric(int dt) {
    return dt_is_integer(dt) || dt == DT_FLOAT || dt == DT_DOUBLE || dt == DT_DECIMAL || dt == DT_NUMERIC;
}
int dt_is_string(int dt) { return dt == DT_CHAR || dt == DT_VARCHAR || dt == DT_TEXT; }
int dt_is_temporal(int dt) { return dt == DT_DATE || dt == DT_TIME || dt == DT_DATETIME; }
int dt_is_bool(int dt) { return dt == DT_BOOLEAN; }

char *datatype_to_sql(int dt, long long len, int scale) {
    switch (dt) {
    case DT_VARCHAR: case DT_CHAR:
        return len > 0 ? xsprintf("%s(%lld)", dtype_name(dt), len) : xstrdup(dtype_name(dt));
    case DT_DECIMAL: case DT_NUMERIC:
        if (len > 0 && scale > 0) return xsprintf("%s(%lld,%d)", dtype_name(dt), len, scale);
        if (len > 0) return xsprintf("%s(%lld)", dtype_name(dt), len);
        return xstrdup(dtype_name(dt));
    default:
        return xstrdup(dtype_name(dt));
    }
}

/* ======================================================================
 *  Construction
 * ====================================================================== */
Node *node_new(NodeType t, int line, int col) {
    Node *n = xcalloc(1, sizeof(Node));
    n->type = t;
    n->line = line;
    n->col = col;
    n->slot = -1;
    n->bind_sid = -1;
    return n;
}
Node *node_add(Node *p, Node *k) {
    if (p->nkids == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 4;
        p->kids = xrealloc(p->kids, sizeof(Node *) * (size_t)p->cap);
    }
    p->kids[p->nkids++] = k;
    return p;
}
Node *node_kid(Node *n, int i) { return (n && i < n->nkids) ? n->kids[i] : NULL; }
Node *mk_list(void) { return node_new(N_LIST, 0, 0); }
Node *mk_ident(const char *name, int line, int col) {
    Node *n = node_new(N_IDENT, line, col);
    n->s1 = xstrdup(name);
    return n;
}
Node *mk_binop(int op, Node *a, Node *b) {
    Node *n = node_new(N_BINOP, a ? a->line : 0, a ? a->col : 0);
    n->op = op;
    node_add(n, a);
    node_add(n, b);
    return n;
}
Node *mk_unop(int op, Node *a) {
    Node *n = node_new(N_UNOP, a ? a->line : 0, a ? a->col : 0);
    n->op = op;
    node_add(n, a);
    return n;
}
Node *mk_int(long long v)    { Node *n = node_new(N_INT, 0, 0); n->ival = v; n->dtype = DT_INT; return n; }
Node *mk_float(double v)     { Node *n = node_new(N_FLOAT, 0, 0); n->fval = v; n->dtype = DT_FLOAT; return n; }
Node *mk_string(const char *s){ Node *n = node_new(N_STRING, 0, 0); n->s1 = xstrdup(s); n->dtype = DT_VARCHAR; return n; }
Node *mk_bool(int v)         { Node *n = node_new(N_BOOL, 0, 0); n->ival = v ? 1 : 0; n->dtype = DT_BOOLEAN; return n; }
Node *mk_null(void)          { Node *n = node_new(N_NULL, 0, 0); n->dtype = DT_NULL; return n; }

Node *node_clone(Node *n) {
    if (!n) return NULL;
    Node *c = xmalloc(sizeof(Node));
    *c = *n;
    c->s1 = xstrdup(n->s1);
    c->s2 = xstrdup(n->s2);
    c->s3 = xstrdup(n->s3);
    c->rqual = xstrdup(n->rqual);
    c->kids = NULL;
    c->nkids = c->cap = 0;
    c->cache = NULL;
    c->cached = 0;
    c->bind_sid = -1;
    for (int i = 0; i < n->nkids; i++) node_add(c, node_clone(n->kids[i]));
    return c;
}

/* ======================================================================
 *  Names of operators etc.
 * ====================================================================== */
const char *op_symbol(int op) {
    switch (op) {
    case O_ADD: return "+"; case O_SUB: return "-"; case O_MUL: return "*";
    case O_DIV: return "/"; case O_MOD: return "%";
    case O_EQ: return "="; case O_NE: return "<>"; case O_LT: return "<";
    case O_GT: return ">"; case O_LE: return "<="; case O_GE: return ">=";
    case O_AND: return "AND"; case O_OR: return "OR"; case O_NOT: return "NOT";
    case O_NEG: return "-"; case O_POS: return "+";
    }
    return "?";
}
const char *agg_name(int k) {
    switch (k) {
    case AGG_COUNT_STAR: case AGG_COUNT: return "COUNT";
    case AGG_SUM: return "SUM"; case AGG_AVG: return "AVG";
    case AGG_MIN: return "MIN"; case AGG_MAX: return "MAX";
    }
    return "?";
}
const char *join_name(int k) {
    switch (k) {
    case J_INNER: return "INNER JOIN"; case J_LEFT: return "LEFT OUTER JOIN";
    case J_RIGHT: return "RIGHT OUTER JOIN"; case J_FULL: return "FULL OUTER JOIN";
    case J_CROSS: return "CROSS JOIN";
    }
    return "JOIN";
}
const char *setop_name(int k, int all) {
    switch (k) {
    case S_UNION: return all ? "UNION ALL" : "UNION";
    case S_INTERSECT: return all ? "INTERSECT ALL" : "INTERSECT";
    case S_EXCEPT: return all ? "EXCEPT ALL" : "EXCEPT";
    }
    return "?";
}
char *priv_mask_str(int m) {
    SB b; sb_init(&b);
    if ((m & P_ALL) == P_ALL) { sb_add(&b, "ALL"); return sb_take(&b); }
    const char *nm[] = {"SELECT", "INSERT", "UPDATE", "DELETE"};
    int first = 1;
    for (int i = 0; i < 4; i++)
        if (m & (1 << i)) { sb_add(&b, "%s%s", first ? "" : ", ", nm[i]); first = 0; }
    return sb_take(&b);
}

const char *stmt_category(Node *s) {
    if (!s) return "";
    switch (s->type) {
    case N_CREATE_DATABASE: case N_DROP_DATABASE: case N_CREATE_TABLE: case N_CREATE_VIEW:
    case N_CREATE_INDEX: case N_ALTER_TABLE: case N_DROP_TABLE: case N_DROP_VIEW:
    case N_DROP_INDEX: case N_TRUNCATE:
        return "DDL";
    case N_INSERT: case N_UPDATE: case N_DELETE: return "DML";
    case N_QUERY: return "DQL";
    case N_GRANT: case N_REVOKE: case N_CREATE_USER: case N_DROP_USER: case N_CONNECT: return "DCL";
    case N_BEGIN: case N_COMMIT: case N_ROLLBACK: case N_SAVEPOINT: case N_ROLLBACK_TO: case N_RELEASE:
        return "TCL";
    default: return "UTILITY";
    }
}
const char *stmt_name(Node *s) {
    switch (s->type) {
    case N_CREATE_DATABASE: return "CREATE DATABASE";
    case N_DROP_DATABASE: return "DROP DATABASE";
    case N_USE: return "USE";
    case N_CREATE_TABLE: return "CREATE TABLE";
    case N_CREATE_VIEW: return "CREATE VIEW";
    case N_CREATE_INDEX: return "CREATE INDEX";
    case N_CREATE_USER: return "CREATE USER";
    case N_DROP_USER: return "DROP USER";
    case N_ALTER_TABLE: return "ALTER TABLE";
    case N_DROP_TABLE: return "DROP TABLE";
    case N_DROP_VIEW: return "DROP VIEW";
    case N_DROP_INDEX: return "DROP INDEX";
    case N_TRUNCATE: return "TRUNCATE TABLE";
    case N_INSERT: return "INSERT";
    case N_UPDATE: return "UPDATE";
    case N_DELETE: return "DELETE";
    case N_QUERY: return "SELECT";
    case N_GRANT: return "GRANT";
    case N_REVOKE: return "REVOKE";
    case N_CONNECT: return "CONNECT";
    case N_BEGIN: return "START TRANSACTION";
    case N_COMMIT: return "COMMIT";
    case N_ROLLBACK: return "ROLLBACK";
    case N_SAVEPOINT: return "SAVEPOINT";
    case N_ROLLBACK_TO: return "ROLLBACK TO SAVEPOINT";
    case N_RELEASE: return "RELEASE SAVEPOINT";
    case N_SHOW: return "SHOW";
    case N_DESCRIBE: return "DESCRIBE";
    case N_EXPLAIN: return "EXPLAIN";
    default: return "STATEMENT";
    }
}

/* ======================================================================
 *  Un-parser: AST -> SQL text
 * ====================================================================== */
static int expr_prec(Node *e) {
    if (!e) return 9;
    switch (e->type) {
    case N_BINOP:
        switch (e->op) {
        case O_OR: return 1;
        case O_AND: return 2;
        case O_ADD: case O_SUB: return 5;
        case O_MUL: case O_DIV: case O_MOD: return 6;
        default: return 4;               /* comparisons */
        }
    case N_UNOP: return e->op == O_NOT ? 3 : 7;
    case N_BETWEEN: case N_IN_LIST: case N_IN_SUBQ: case N_LIKE: case N_IS_NULL: return 4;
    default: return 9;
    }
}

static void unparse_query(SB *b, Node *q);
static void unparse_expr(SB *b, Node *e, int parent_prec);

static void unparse_string(SB *b, const char *s) {
    sb_addc(b, '\'');
    for (; *s; s++) {
        if (*s == '\'') sb_addc(b, '\'');
        sb_addc(b, *s);
    }
    sb_addc(b, '\'');
}

static void unparse_subq(SB *b, Node *q) {
    if (g_unparse_subq_ref && q && q->subq_id > 0) { sb_add(b, "SUBQ#%d", q->subq_id); return; }
    sb_addc(b, '(');
    unparse_query(b, q);
    sb_addc(b, ')');
}

static void unparse_expr(SB *b, Node *e, int pp) {
    if (!e) { sb_add(b, "?"); return; }
    int p = expr_prec(e);
    int paren = p < pp;
    if (paren) sb_addc(b, '(');
    char buf[64];
    switch (e->type) {
    case N_INT: sb_add(b, "%lld", e->ival); break;
    case N_FLOAT: fmt_double(e->fval, buf, sizeof buf); sb_add(b, "%s", buf); break;
    case N_STRING: unparse_string(b, e->s1); break;
    case N_DATE: sb_add(b, "DATE "); unparse_string(b, e->s1); break;
    case N_NULL: sb_add(b, "NULL"); break;
    case N_BOOL: sb_add(b, e->ival ? "TRUE" : "FALSE"); break;
    case N_COLREF:
        if (g_unparse_resolved && e->rqual) sb_add(b, "%s.%s", e->rqual, e->s2);
        else if (e->s1) sb_add(b, "%s.%s", e->s1, e->s2);
        else sb_add(b, "%s", e->s2);
        break;
    case N_STAR:
        if (e->s1) sb_add(b, "%s.*", e->s1); else sb_add(b, "*");
        break;
    case N_BINOP: {
        int rp = p + 1;
        unparse_expr(b, e->kids[0], (p == 4) ? p + 1 : p);
        sb_add(b, " %s ", op_symbol(e->op));
        unparse_expr(b, e->kids[1], rp);
        break;
    }
    case N_UNOP:
        if (e->op == O_NOT) { sb_add(b, "NOT "); unparse_expr(b, e->kids[0], p); }
        else { sb_add(b, "%s", op_symbol(e->op)); unparse_expr(b, e->kids[0], p); }
        break;
    case N_FUNC:
        sb_add(b, "%s(", e->s1);
        for (int i = 0; i < e->nkids; i++) {
            if (i) sb_add(b, ", ");
            unparse_expr(b, e->kids[i], 0);
        }
        sb_addc(b, ')');
        break;
    case N_AGG:
        sb_add(b, "%s(", agg_name(e->op));
        if (e->op == AGG_COUNT_STAR) sb_addc(b, '*');
        else {
            if (e->flag) sb_add(b, "DISTINCT ");
            unparse_expr(b, e->kids[0], 0);
        }
        sb_addc(b, ')');
        break;
    case N_BETWEEN:
        unparse_expr(b, e->kids[0], p + 1);
        sb_add(b, e->flag ? " NOT BETWEEN " : " BETWEEN ");
        unparse_expr(b, e->kids[1], 5);
        sb_add(b, " AND ");
        unparse_expr(b, e->kids[2], 5);
        break;
    case N_IN_LIST:
        unparse_expr(b, e->kids[0], p + 1);
        sb_add(b, e->flag ? " NOT IN (" : " IN (");
        for (int i = 0; i < e->kids[1]->nkids; i++) {
            if (i) sb_add(b, ", ");
            unparse_expr(b, e->kids[1]->kids[i], 0);
        }
        sb_addc(b, ')');
        break;
    case N_IN_SUBQ:
        unparse_expr(b, e->kids[0], p + 1);
        sb_add(b, e->flag ? " NOT IN " : " IN ");
        unparse_subq(b, e->kids[1]);
        break;
    case N_LIKE:
        unparse_expr(b, e->kids[0], p + 1);
        sb_add(b, e->flag ? " NOT LIKE " : " LIKE ");
        unparse_expr(b, e->kids[1], p + 1);
        break;
    case N_IS_NULL:
        unparse_expr(b, e->kids[0], p + 1);
        sb_add(b, e->flag ? " IS NOT NULL" : " IS NULL");
        break;
    case N_SUBQUERY:
        unparse_subq(b, e->kids[0]);
        break;
    case N_EXISTS:
        sb_add(b, "EXISTS ");
        unparse_subq(b, e->kids[0]);
        break;
    default:
        sb_add(b, "<expr>");
    }
    if (paren) sb_addc(b, ')');
}

char *expr_to_sql(Node *e) {
    SB b; sb_init(&b);
    unparse_expr(&b, e, 0);
    return sb_take(&b);
}

static void unparse_from(SB *b, Node *t) {
    if (!t) return;
    switch (t->type) {
    case N_TABLE_REF:
        sb_add(b, "%s", t->s1);
        if (t->s2) sb_add(b, " AS %s", t->s2);
        break;
    case N_DERIVED_TABLE:
        sb_addc(b, '(');
        unparse_query(b, t->kids[0]);
        sb_add(b, ") AS %s", t->s2);
        break;
    case N_JOIN:
        unparse_from(b, t->kids[0]);
        if (t->op == J_CROSS && t->flag) sb_add(b, ", ");
        else sb_add(b, " %s ", join_name(t->op));
        unparse_from(b, t->kids[1]);
        if (t->kids[2] && t->op != J_CROSS) {
            sb_add(b, " ON ");
            unparse_expr(b, t->kids[2], 0);
        }
        break;
    default: break;
    }
}

static void unparse_body(SB *b, Node *s) {
    if (s->type == N_SETOP) {
        unparse_body(b, s->kids[0]);
        sb_add(b, " %s ", setop_name(s->op, s->flag));
        unparse_body(b, s->kids[1]);
        return;
    }
    sb_add(b, "SELECT ");
    if (s->flag) sb_add(b, "DISTINCT ");
    Node *items = s->kids[0];
    for (int i = 0; i < items->nkids; i++) {
        Node *it = items->kids[i];
        if (i) sb_add(b, ", ");
        if (it->type == N_STAR) unparse_expr(b, it, 0);
        else {
            unparse_expr(b, it->kids[0], 0);
            if (it->s1) sb_add(b, " AS %s", it->s1);
        }
    }
    if (s->kids[1]) { sb_add(b, " FROM "); unparse_from(b, s->kids[1]); }
    if (s->kids[2]) { sb_add(b, " WHERE "); unparse_expr(b, s->kids[2], 0); }
    if (s->kids[3]) {
        sb_add(b, " GROUP BY ");
        for (int i = 0; i < s->kids[3]->nkids; i++) {
            if (i) sb_add(b, ", ");
            unparse_expr(b, s->kids[3]->kids[i], 0);
        }
    }
    if (s->kids[4]) { sb_add(b, " HAVING "); unparse_expr(b, s->kids[4], 0); }
}

static void unparse_query(SB *b, Node *q) {
    if (!q) return;
    if (q->type != N_QUERY) { unparse_body(b, q); return; }
    unparse_body(b, q->kids[0]);
    Node *ord = node_kid(q, 1);
    if (ord) {
        sb_add(b, " ORDER BY ");
        for (int i = 0; i < ord->nkids; i++) {
            if (i) sb_add(b, ", ");
            unparse_expr(b, ord->kids[i]->kids[0], 0);
            if (ord->kids[i]->flag) sb_add(b, " DESC");
        }
    }
    if (node_kid(q, 2)) sb_add(b, " LIMIT %lld", q->kids[2]->ival);
    if (node_kid(q, 3)) sb_add(b, " OFFSET %lld", q->kids[3]->ival);
}

char *query_to_sql(Node *q) {
    SB b; sb_init(&b);
    unparse_query(&b, q);
    return sb_take(&b);
}

/* ======================================================================
 *  Pretty tree printer.  The AST is first converted to a "display tree"
 *  of labelled nodes, which is then drawn with box characters.
 * ====================================================================== */
typedef struct DNode {
    char *label;
    struct DNode **kids;
    int n, cap;
} DNode;

static DNode *dn(const char *fmt, ...) {
    DNode *d = xcalloc(1, sizeof(DNode));
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    d->label = xstrdup(buf);
    return d;
}
static DNode *dn_add(DNode *p, DNode *k) {
    if (!k) return p;
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 4;
        p->kids = xrealloc(p->kids, sizeof(DNode *) * (size_t)p->cap);
    }
    p->kids[p->n++] = k;
    return p;
}
static void dn_free(DNode *d) {
    if (!d) return;
    for (int i = 0; i < d->n; i++) dn_free(d->kids[i]);
    free(d->kids);
    free(d->label);
    free(d);
}

static DNode *d_expr(Node *e);
static DNode *d_query(Node *q);

static DNode *d_expr(Node *e) {
    if (!e) return dn("<null>");
    char *s;
    DNode *d;
    switch (e->type) {
    case N_INT: case N_FLOAT: case N_STRING: case N_DATE: case N_NULL: case N_BOOL:
    case N_COLREF: case N_STAR:
        s = expr_to_sql(e);
        d = dn("%s", s);
        free(s);
        return d;
    case N_BINOP:
        d = dn("%s", op_symbol(e->op));
        dn_add(d, d_expr(e->kids[0]));
        dn_add(d, d_expr(e->kids[1]));
        return d;
    case N_UNOP:
        d = dn("%s", e->op == O_NOT ? "NOT" : (e->op == O_NEG ? "NEGATE (-)" : "+"));
        dn_add(d, d_expr(e->kids[0]));
        return d;
    case N_FUNC:
        d = dn("FUNCTION %s()", e->s1);
        for (int i = 0; i < e->nkids; i++) dn_add(d, d_expr(e->kids[i]));
        return d;
    case N_AGG:
        if (e->op == AGG_COUNT_STAR) return dn("AGGREGATE COUNT(*)");
        d = dn("AGGREGATE %s%s", agg_name(e->op), e->flag ? " DISTINCT" : "");
        dn_add(d, d_expr(e->kids[0]));
        return d;
    case N_BETWEEN:
        d = dn(e->flag ? "NOT BETWEEN" : "BETWEEN");
        dn_add(d, d_expr(e->kids[0]));
        dn_add(d, d_expr(e->kids[1]));
        dn_add(d, d_expr(e->kids[2]));
        return d;
    case N_IN_LIST: {
        d = dn(e->flag ? "NOT IN" : "IN");
        dn_add(d, d_expr(e->kids[0]));
        DNode *l = dn("LIST");
        for (int i = 0; i < e->kids[1]->nkids; i++) dn_add(l, d_expr(e->kids[1]->kids[i]));
        dn_add(d, l);
        return d;
    }
    case N_IN_SUBQ: {
        d = dn(e->flag ? "NOT IN" : "IN");
        dn_add(d, d_expr(e->kids[0]));
        DNode *sq = dn("SUBQUERY");
        dn_add(sq, d_query(e->kids[1]));
        dn_add(d, sq);
        return d;
    }
    case N_LIKE:
        d = dn(e->flag ? "NOT LIKE" : "LIKE");
        dn_add(d, d_expr(e->kids[0]));
        dn_add(d, d_expr(e->kids[1]));
        return d;
    case N_IS_NULL:
        d = dn(e->flag ? "IS NOT NULL" : "IS NULL");
        dn_add(d, d_expr(e->kids[0]));
        return d;
    case N_SUBQUERY:
        d = dn("SUBQUERY");
        dn_add(d, d_query(e->kids[0]));
        return d;
    case N_EXISTS:
        d = dn("EXISTS");
        dn_add(d, d_query(e->kids[0]));
        return d;
    default:
        return dn("<expr>");
    }
}

/* "label" for a leaf or "label" node with the expression as child */
static DNode *d_labelled(const char *prefix, Node *e, const char *suffix) {
    DNode *x = d_expr(e);
    if (x->n == 0) {
        DNode *r = dn("%s%s%s", prefix ? prefix : "", x->label, suffix ? suffix : "");
        dn_free(x);
        return r;
    }
    DNode *r = dn("%s%s", prefix ? prefix : "", suffix ? suffix + 1 : "EXPR");
    dn_add(r, x);
    return r;
}

static DNode *d_from(Node *t) {
    if (!t) return NULL;
    DNode *d;
    switch (t->type) {
    case N_TABLE_REF:
        if (t->s2) return dn("%s AS %s", t->s1, t->s2);
        return dn("%s", t->s1);
    case N_DERIVED_TABLE:
        d = dn("DERIVED TABLE AS %s", t->s2);
        dn_add(d, d_query(t->kids[0]));
        return d;
    case N_JOIN:
        d = dn("%s", (t->op == J_CROSS && t->flag) ? "CROSS JOIN (,)" : join_name(t->op));
        dn_add(d, d_from(t->kids[0]));
        dn_add(d, d_from(t->kids[1]));
        if (t->kids[2] && t->op != J_CROSS) {
            DNode *on = dn("ON");
            dn_add(on, d_expr(t->kids[2]));
            dn_add(d, on);
        }
        return d;
    default:
        return dn("?");
    }
}

static DNode *d_body(Node *s) {
    DNode *d;
    if (s->type == N_SETOP) {
        d = dn("%s", setop_name(s->op, s->flag));
        dn_add(d, d_body(s->kids[0]));
        dn_add(d, d_body(s->kids[1]));
        return d;
    }
    d = dn(s->flag ? "SELECT DISTINCT" : "SELECT");
    DNode *cols = dn("COLUMNS");
    Node *items = s->kids[0];
    for (int i = 0; i < items->nkids; i++) {
        Node *it = items->kids[i];
        if (it->type == N_STAR) { dn_add(cols, d_expr(it)); continue; }
        if (it->s1) {
            char *suf = xsprintf(" AS %s", it->s1);
            DNode *x = d_expr(it->kids[0]);
            if (x->n == 0) { dn_add(cols, dn("%s%s", x->label, suf)); dn_free(x); }
            else { DNode *a = dn("AS %s", it->s1); dn_add(a, x); dn_add(cols, a); }
            free(suf);
        } else dn_add(cols, d_expr(it->kids[0]));
    }
    dn_add(d, cols);
    if (s->kids[1]) { DNode *f = dn("FROM"); dn_add(f, d_from(s->kids[1])); dn_add(d, f); }
    if (s->kids[2]) { DNode *w = dn("WHERE"); dn_add(w, d_expr(s->kids[2])); dn_add(d, w); }
    if (s->kids[3]) {
        DNode *g = dn("GROUP BY");
        for (int i = 0; i < s->kids[3]->nkids; i++) dn_add(g, d_expr(s->kids[3]->kids[i]));
        dn_add(d, g);
    }
    if (s->kids[4]) { DNode *h = dn("HAVING"); dn_add(h, d_expr(s->kids[4])); dn_add(d, h); }
    return d;
}

static DNode *d_query(Node *q) {
    if (q->type != N_QUERY) return d_body(q);
    DNode *d = d_body(q->kids[0]);
    Node *ord = node_kid(q, 1);
    if (ord) {
        DNode *o = dn("ORDER BY");
        for (int i = 0; i < ord->nkids; i++)
            dn_add(o, d_labelled(NULL, ord->kids[i]->kids[0], ord->kids[i]->flag ? " DESC" : " ASC"));
        dn_add(d, o);
    }
    if (node_kid(q, 2)) dn_add(d, dn("LIMIT %lld", q->kids[2]->ival));
    if (node_kid(q, 3)) dn_add(d, dn("OFFSET %lld", q->kids[3]->ival));
    return d;
}

static DNode *d_coldef(Node *c) {
    DNode *d = dn("%s", c->s1);
    Node *t = c->kids[0];
    char *ts = datatype_to_sql(t->op, t->ival, t->flag);
    dn_add(d, dn("%s", ts));
    free(ts);
    Node *cons = c->kids[1];
    for (int i = 0; cons && i < cons->nkids; i++) {
        Node *k = cons->kids[i];
        switch (k->op) {
        case C_PRIMARY: dn_add(d, dn("PRIMARY KEY")); break;
        case C_NOTNULL: dn_add(d, dn("NOT NULL")); break;
        case C_NULL:    dn_add(d, dn("NULL")); break;
        case C_UNIQUE:  dn_add(d, dn("UNIQUE")); break;
        case C_DEFAULT: dn_add(d, d_labelled("DEFAULT ", k->kids[0], NULL)); break;
        case C_CHECK: { DNode *ch = dn("CHECK"); dn_add(ch, d_expr(k->kids[0])); dn_add(d, ch); break; }
        case C_FOREIGN: dn_add(d, dn("REFERENCES %s(%s)", k->s1, k->s2)); break;
        }
    }
    return d;
}

static char *ident_list_str(Node *l) {
    SB b; sb_init(&b);
    for (int i = 0; l && i < l->nkids; i++) sb_add(&b, "%s%s", i ? ", " : "", l->kids[i]->s1);
    return sb_take(&b);
}

static DNode *d_tcons(Node *k) {
    char *cols = ident_list_str(k->kids[0]);
    DNode *d = NULL;
    switch (k->op) {
    case C_PRIMARY: d = dn("PRIMARY KEY (%s)", cols); break;
    case C_UNIQUE:  d = dn("UNIQUE (%s)", cols); break;
    case C_FOREIGN: {
        char *rc = ident_list_str(k->kids[1]);
        d = dn("FOREIGN KEY (%s) REFERENCES %s(%s)", cols, k->s1, rc);
        free(rc);
        break;
    }
    case C_CHECK: d = dn("CHECK"); dn_add(d, d_expr(k->kids[2])); break;
    }
    free(cols);
    return d;
}

static DNode *d_stmt(Node *s) {
    DNode *d, *x;
    char *tmp;
    switch (s->type) {
    case N_QUERY: return d_query(s);
    case N_CREATE_DATABASE: d = dn("CREATE DATABASE"); dn_add(d, dn("%s", s->s1)); return d;
    case N_DROP_DATABASE:   d = dn("DROP DATABASE%s", s->flag ? " IF EXISTS" : ""); dn_add(d, dn("%s", s->s1)); return d;
    case N_USE:             d = dn("USE DATABASE"); dn_add(d, dn("%s", s->s1)); return d;
    case N_CREATE_TABLE:
        d = dn("CREATE TABLE%s", s->flag ? " IF NOT EXISTS" : "");
        x = dn("%s", s->s1);
        for (int i = 0; i < s->nkids; i++)
            dn_add(x, s->kids[i]->type == N_COLUMN_DEF ? d_coldef(s->kids[i]) : d_tcons(s->kids[i]));
        dn_add(d, x);
        return d;
    case N_CREATE_VIEW:
        d = dn("CREATE VIEW");
        x = dn("%s", s->s1);
        DNode *as = dn("AS");
        dn_add(as, d_query(s->kids[0]));
        dn_add(x, as);
        dn_add(d, x);
        return d;
    case N_CREATE_INDEX:
        d = dn("CREATE %sINDEX", s->flag ? "UNIQUE " : "");
        dn_add(d, dn("NAME %s", s->s1));
        dn_add(d, dn("ON TABLE %s", s->s2));
        x = dn("COLUMNS");
        for (int i = 0; i < s->kids[0]->nkids; i++) dn_add(x, dn("%s", s->kids[0]->kids[i]->s1));
        dn_add(d, x);
        return d;
    case N_CREATE_USER: d = dn("CREATE USER"); dn_add(d, dn("%s", s->s1)); return d;
    case N_DROP_USER:   d = dn("DROP USER"); dn_add(d, dn("%s", s->s1)); return d;
    case N_ALTER_TABLE: {
        d = dn("ALTER TABLE");
        x = dn("%s", s->s1);
        Node *a = s->kids[0];
        DNode *ad;
        switch (a->op) {
        case A_ADD: ad = dn("ADD COLUMN"); dn_add(ad, d_coldef(a->kids[0])); break;
        case A_MODIFY: ad = dn("MODIFY COLUMN"); dn_add(ad, d_coldef(a->kids[0])); break;
        case A_DROP: ad = dn("DROP COLUMN"); dn_add(ad, dn("%s", a->s1)); break;
        case A_RENAME_COL: ad = dn("RENAME COLUMN"); dn_add(ad, dn("%s", a->s1)); dn_add(ad, dn("TO %s", a->s2)); break;
        default: ad = dn("RENAME TABLE"); dn_add(ad, dn("TO %s", a->s2)); break;
        }
        dn_add(x, ad);
        dn_add(d, x);
        return d;
    }
    case N_DROP_TABLE: d = dn("DROP TABLE%s", s->flag ? " IF EXISTS" : ""); dn_add(d, dn("%s", s->s1)); return d;
    case N_DROP_VIEW:  d = dn("DROP VIEW%s", s->flag ? " IF EXISTS" : ""); dn_add(d, dn("%s", s->s1)); return d;
    case N_DROP_INDEX: d = dn("DROP INDEX%s", s->flag ? " IF EXISTS" : ""); dn_add(d, dn("%s", s->s1)); return d;
    case N_TRUNCATE:   d = dn("TRUNCATE TABLE"); dn_add(d, dn("%s", s->s1)); return d;
    case N_INSERT: {
        d = dn("INSERT");
        dn_add(d, dn("INTO %s", s->s1));
        if (s->kids[0]) {
            x = dn("COLUMNS");
            for (int i = 0; i < s->kids[0]->nkids; i++) dn_add(x, dn("%s", s->kids[0]->kids[i]->s1));
            dn_add(d, x);
        }
        Node *src = s->kids[1];
        if (src->type == N_QUERY) { x = dn("SOURCE QUERY"); dn_add(x, d_query(src)); dn_add(d, x); }
        else {
            x = dn("VALUES");
            for (int r = 0; r < src->nkids; r++) {
                DNode *row = dn("ROW %d", r + 1);
                for (int i = 0; i < src->kids[r]->nkids; i++) dn_add(row, d_expr(src->kids[r]->kids[i]));
                dn_add(x, row);
            }
            dn_add(d, x);
        }
        return d;
    }
    case N_UPDATE: {
        d = dn("UPDATE");
        dn_add(d, dn("TABLE %s", s->s1));
        x = dn("SET");
        for (int i = 0; i < s->kids[0]->nkids; i++) {
            Node *a = s->kids[0]->kids[i];
            DNode *eq = dn("=");
            dn_add(eq, dn("%s", a->s1));
            dn_add(eq, d_expr(a->kids[0]));
            dn_add(x, eq);
        }
        dn_add(d, x);
        if (s->kids[1]) { x = dn("WHERE"); dn_add(x, d_expr(s->kids[1])); dn_add(d, x); }
        return d;
    }
    case N_DELETE:
        d = dn("DELETE");
        dn_add(d, dn("FROM %s", s->s1));
        if (s->kids[0]) { x = dn("WHERE"); dn_add(x, d_expr(s->kids[0])); dn_add(d, x); }
        return d;
    case N_GRANT: case N_REVOKE: {
        d = dn(s->type == N_GRANT ? "GRANT" : "REVOKE");
        x = dn("PRIVILEGES");
        for (int i = 0; i < s->kids[0]->nkids; i++) {
            tmp = priv_mask_str(s->kids[0]->kids[i]->op);
            dn_add(x, dn("%s", tmp));
            free(tmp);
        }
        dn_add(d, x);
        dn_add(d, dn("ON %s", s->s1));
        x = dn(s->type == N_GRANT ? "TO" : "FROM");
        for (int i = 0; i < s->kids[1]->nkids; i++) dn_add(x, dn("%s", s->kids[1]->kids[i]->s1));
        dn_add(d, x);
        return d;
    }
    case N_CONNECT:     d = dn("CONNECT"); dn_add(d, dn("USER %s", s->s1)); return d;
    case N_BEGIN:       return dn("START TRANSACTION");
    case N_COMMIT:      return dn("COMMIT");
    case N_ROLLBACK:    return dn("ROLLBACK");
    case N_SAVEPOINT:   d = dn("SAVEPOINT"); dn_add(d, dn("%s", s->s1)); return d;
    case N_ROLLBACK_TO: d = dn("ROLLBACK TO SAVEPOINT"); dn_add(d, dn("%s", s->s1)); return d;
    case N_RELEASE:     d = dn("RELEASE SAVEPOINT"); dn_add(d, dn("%s", s->s1)); return d;
    case N_SHOW: {
        const char *w[] = {"", "TABLES", "DATABASES", "USERS", "GRANTS", "VIEWS", "INDEXES", "TRANSACTION"};
        d = dn("SHOW %s", w[s->op]);
        if (s->s1) dn_add(d, dn("FOR %s", s->s1));
        return d;
    }
    case N_DESCRIBE: d = dn("DESCRIBE"); dn_add(d, dn("%s", s->s1)); return d;
    case N_EXPLAIN:  d = dn("EXPLAIN"); dn_add(d, d_stmt(s->kids[0])); return d;
    default: return dn("<statement>");
    }
}

static void dn_print(DNode *d, const char *prefix, int last, int root) {
    if (root) printf("%s\n", d->label);
    else printf("%s%s%s\n", prefix, tree_branch(last), d->label);
    char *np = root ? xstrdup("") : xsprintf("%s%s", prefix, tree_indent(last));
    for (int i = 0; i < d->n; i++) dn_print(d->kids[i], np, i == d->n - 1, 0);
    free(np);
}

void ast_print(Node *root) {
    DNode *d = d_stmt(root);
    dn_print(d, "", 1, 1);
    dn_free(d);
}
