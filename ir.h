/*
 * ir.h - Intermediate Representation
 *
 * The validated AST is translated into a tree of relational-algebra
 * operations. It is printed as three-address style code, one temporary
 * per operation:
 *
 *     T1 = LOAD students
 *     T2 = FILTER T1 WHERE age > 18
 *     T3 = PROJECT T2 (name, age)
 *
 * The optimizer rewrites this tree and the executor interprets it.
 */
#ifndef IR_H
#define IR_H

#include "ast.h"
#include "util.h"

typedef enum {
    IR_LOAD,       /* scan a base table                       */
    IR_VIEW,       /* expand a view (sub-plan)                */
    IR_DERIVED,    /* subquery in FROM (sub-plan)             */
    IR_DUAL,       /* one empty row (SELECT without FROM)     */
    IR_VALUES,     /* literal rows of INSERT ... VALUES       */
    IR_FILTER,     /* selection (WHERE / HAVING / pushed)     */
    IR_JOIN,
    IR_GROUP,
    IR_AGGREGATE,
    IR_PROJECT,
    IR_DISTINCT,
    IR_SORT,
    IR_LIMIT,
    IR_SETOP,      /* UNION / INTERSECT / EXCEPT              */
    IR_INSERT,
    IR_UPDATE,
    IR_DELETE,
    IR_COMMAND     /* DDL / DCL / TCL / utility operation     */
} IROp;

enum { FK_WHERE = 1, FK_HAVING, FK_PUSHED };

typedef struct IRNode {
    IROp   op;
    int    id;
    struct IRNode *left, *right;

    char  *table;          /* LOAD / VIEW / DML target              */
    char  *alias;          /* range name                            */
    char **cols;           /* LOAD: columns to read (NULL = all)    */
    int    ncols;
    int    no_prune;

    Node  *pred;           /* FILTER predicate / JOIN condition     */
    int    join_type;
    int    filter_kind;

    Node **exprs;          /* PROJECT expressions (visible + hidden) */
    char **names;
    int    nexprs, nvisible;
    int    trim;           /* PROJECT: keep first nvisible columns  */

    Node **keys;  int nkeys;     /* GROUP / AGGREGATE */
    Node **aggs;  int naggs;     /* AGGREGATE         */

    int   *sort_cols, *sort_desc;
    char **sort_names;
    int    nsort;

    long long limit, offset;
    int    setop, set_all;
    int    post_agg;

    Node  *ast;            /* statement / view query / derived query */
    struct IRNode *sub;    /* VIEW / DERIVED sub-plan                */
    char **out_names;      /* VIEW / DERIVED output column names     */
    int    nout;
    char  *note;           /* optimizer annotation                   */
    Node  *hkey_l, *hkey_r;/* JOIN: hash-join keys chosen by optimizer */
} IRNode;

typedef struct {
    Node   *holder;        /* N_SUBQUERY / N_IN_SUBQ / N_EXISTS      */
    Node   *query;
    IRNode *root;
    int     id;
} SubPlan;

typedef struct Plan {
    Node   *stmt;
    IRNode *root;
    PtrVec  subplans;      /* SubPlan*                               */
    int     explain;
} Plan;

Plan   *ir_generate(Node *stmt);
IRNode *ir_new(IROp op);
void    ir_print(Plan *p);
int     ir_count(IRNode *n);
const char *ir_op_name(IROp op);

#endif
