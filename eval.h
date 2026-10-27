/*
 * eval.h - run-time values, relations and expression evaluation
 *
 * A Relation is an in-memory table (rows of Values) produced by each IR
 * instruction. Expressions from the AST are evaluated against a row
 * through an Env chain (the chain gives correlated sub-queries access
 * to the rows of enclosing queries).
 */
#ifndef EVAL_H
#define EVAL_H

#include "ast.h"
#include <setjmp.h>

typedef enum { V_NULL = 0, V_INT, V_FLOAT, V_STR, V_BOOL } VKind;

typedef struct {
    VKind     k;
    long long i;
    double    f;
    char     *s;
} Value;

Value v_null(void);
Value v_int(long long i);
Value v_float(double f);
Value v_str(const char *s);
Value v_bool(int b);
Value v_copy(Value v);
void  v_free(Value *v);
int   v_is_null(Value v);
int   v_truth(Value v);                  /* 1 true, 0 false, -1 unknown (NULL) */
int   v_compare(Value a, Value b);       /* <0, 0, >0 (both non-NULL)         */
int   v_same(Value a, Value b);          /* equality where NULL == NULL        */
double v_num(Value v);
char *v_to_text(Value v, int dtype);     /* display text                       */
char *v_to_sql(Value v);                 /* as a SQL literal                   */
int   like_match(const char *s, const char *pat);

typedef struct {
    char     *qual;      /* range name (table or alias), may be NULL */
    char     *name;
    int       dtype;
    long long dlen;
} ColInfo;

typedef struct Relation {
    int        ncols;
    ColInfo   *cols;
    int        nrows, cap;
    Value    **rows;
    long long *rowids;       /* storage row ids (only for base-table rows) */
    int        has_rowids;
    int       *group_start;  /* GROUP: first row of each group             */
    int        ngroups;
    int        sid;          /* schema id used for column-binding caches   */
} Relation;

Relation *rel_new(int ncols);
void      rel_set_col(Relation *r, int i, const char *qual, const char *name, int dtype, long long len);
void      rel_add_row(Relation *r, Value *row, long long rowid);
void      rel_free(Relation *r);
int       rel_find_col(Relation *r, const char *qual, const char *name);
Value    *row_new(int n);

typedef struct Env {
    Relation   *rel;
    Value      *row;
    int         post_agg;    /* row is an aggregated row (slots valid) */
    struct Env *outer;
} Env;

/* errors raised while evaluating jump back to the executor */
extern jmp_buf *g_exec_jmp;
extern char     g_exec_errmsg[1024];
extern int      g_exec_errline, g_exec_errcol;
void exec_error(Node *where, const char *fmt, ...) __attribute__((noreturn));

Value eval_expr(Node *e, Env *env);
int   eval_predicate(Node *e, Env *env);    /* WHERE semantics: true only */

/* convert a value to a column's type; returns 0 and fills err on failure */
int coerce_value(Value in, int dtype, long long len, int scale, Value *out, char *err, size_t errn);

/* provided by the executor: run a (sub)query plan */
Relation *exec_subquery(Node *query, Env *outer);

#endif
