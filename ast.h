/*
 * ast.h - Abstract Syntax Tree for SQL-C
 *
 * Every construct is a Node with a type, a few scalar fields and an
 * ordered list of children. The meaning of each field per node type is
 * documented next to the enum. Semantic analysis later decorates the
 * same nodes with type / scope / slot information.
 */
#ifndef AST_H
#define AST_H

#include "util.h"

/* ---------- SQL data types ---------- */
typedef enum {
    DT_UNKNOWN = 0, DT_NULL,
    DT_INT, DT_SMALLINT, DT_BIGINT,
    DT_FLOAT, DT_DOUBLE, DT_DECIMAL, DT_NUMERIC,
    DT_CHAR, DT_VARCHAR, DT_TEXT,
    DT_DATE, DT_TIME, DT_DATETIME,
    DT_BOOLEAN
} DataType;

const char *dtype_name(int dt);
int dt_is_numeric(int dt);
int dt_is_integer(int dt);
int dt_is_string(int dt);
int dt_is_temporal(int dt);
int dt_is_bool(int dt);

/* ---------- node types ---------- */
typedef enum {
    /* statements                              fields used                       */
    N_CREATE_DATABASE, /* s1=name, flag=IF NOT EXISTS                           */
    N_DROP_DATABASE,   /* s1=name, flag=IF EXISTS                               */
    N_USE,             /* s1=database                                           */
    N_CREATE_TABLE,    /* s1=name, flag=IF NOT EXISTS, kids=COLUMN_DEF/TABLE_CONSTRAINT */
    N_CREATE_VIEW,     /* s1=name, kids[0]=QUERY                                */
    N_CREATE_INDEX,    /* s1=index, s2=table, flag=UNIQUE, kids[0]=LIST(IDENT)  */
    N_CREATE_USER,     /* s1=user                                               */
    N_DROP_USER,       /* s1=user                                               */
    N_ALTER_TABLE,     /* s1=table, kids[0]=ALTER_ACTION                        */
    N_DROP_TABLE,      /* s1=name, flag=IF EXISTS                               */
    N_DROP_VIEW,
    N_DROP_INDEX,
    N_TRUNCATE,        /* s1=table                                              */
    N_INSERT,          /* s1=table, kids[0]=LIST(IDENT) or NULL, kids[1]=LIST(rows) or QUERY */
    N_UPDATE,          /* s1=table, kids[0]=LIST(ASSIGN), kids[1]=where         */
    N_DELETE,          /* s1=table, kids[0]=where                               */
    N_QUERY,           /* kids[0]=SELECT|SETOP, kids[1]=LIST(ORDER_ITEM), kids[2]=limit INT, kids[3]=offset INT */
    N_SELECT,          /* flag=DISTINCT, kids: [0]=LIST(items) [1]=from [2]=where [3]=LIST(group) [4]=having */
    N_SETOP,           /* op=S_*, flag=ALL, kids[0]=left, kids[1]=right         */
    N_GRANT,           /* kids[0]=LIST(PRIV), s1=object, kids[1]=LIST(IDENT users) */
    N_REVOKE,
    N_CONNECT,         /* s1=user                                               */
    N_BEGIN, N_COMMIT, N_ROLLBACK,
    N_SAVEPOINT,       /* s1=name                                               */
    N_ROLLBACK_TO,     /* s1=name                                               */
    N_RELEASE,         /* s1=name                                               */
    N_SHOW,            /* op=SH_*, s1=optional name                             */
    N_DESCRIBE,        /* s1=table                                              */
    N_EXPLAIN,         /* kids[0]=statement                                     */

    /* DDL pieces */
    N_COLUMN_DEF,      /* s1=name, kids[0]=DATATYPE, kids[1]=LIST(COL_CONSTRAINT) */
    N_DATATYPE,        /* op=DataType, ival=length/precision, flag=scale        */
    N_COL_CONSTRAINT,  /* op=C_*, kids[0]=expr (DEFAULT/CHECK), s1=ref table, s2=ref col, s3=name */
    N_TABLE_CONSTRAINT,/* op=C_*, kids[0]=LIST cols, kids[1]=LIST ref cols, kids[2]=check expr, s1=ref table, s3=name */
    N_ALTER_ACTION,    /* op=A_*, kids[0]=COLUMN_DEF, s1=column, s2=new name    */

    /* query pieces */
    N_LIST,            /* generic ordered list                                  */
    N_IDENT,           /* s1=name                                               */
    N_SELECT_ITEM,     /* kids[0]=expr, s1=alias                                */
    N_STAR,            /* s1=qualifier or NULL                                  */
    N_TABLE_REF,       /* s1=table, s2=alias                                    */
    N_DERIVED_TABLE,   /* kids[0]=QUERY, s2=alias                               */
    N_JOIN,            /* op=J_*, kids[0]=left, kids[1]=right, kids[2]=condition */
    N_ORDER_ITEM,      /* kids[0]=expr, flag=1 if DESC                          */
    N_ASSIGN,          /* s1=column, kids[0]=expr                               */
    N_PRIV,            /* op=P_* bit                                            */

    /* expressions */
    N_BINOP,           /* op=O_*, kids[0], kids[1]                              */
    N_UNOP,            /* op=O_NOT/O_NEG/O_POS, kids[0]                         */
    N_COLREF,          /* s1=qualifier (may be NULL), s2=column                 */
    N_INT,             /* ival                                                  */
    N_FLOAT,           /* fval                                                  */
    N_STRING,          /* s1                                                    */
    N_DATE,            /* s1 = 'YYYY-MM-DD'                                     */
    N_NULL,
    N_BOOL,            /* ival 0/1                                              */
    N_FUNC,            /* s1=name, kids=args (scalar function)                  */
    N_AGG,             /* op=AGG_*, flag=DISTINCT, kids[0]=arg (NULL for COUNT(*)) */
    N_BETWEEN,         /* flag=NOT, kids[0] BETWEEN kids[1] AND kids[2]         */
    N_IN_LIST,         /* flag=NOT, kids[0], kids[1]=LIST                       */
    N_IN_SUBQ,         /* flag=NOT, kids[0], kids[1]=QUERY                      */
    N_LIKE,            /* flag=NOT, kids[0], kids[1]                            */
    N_IS_NULL,         /* flag=NOT, kids[0]                                     */
    N_SUBQUERY,        /* kids[0]=QUERY (scalar subquery)                       */
    N_EXISTS           /* kids[0]=QUERY                                         */
} NodeType;

/* expression operators */
enum { O_ADD = 1, O_SUB, O_MUL, O_DIV, O_MOD,
       O_EQ, O_NE, O_LT, O_GT, O_LE, O_GE,
       O_AND, O_OR, O_NOT, O_NEG, O_POS };
/* aggregate kinds */
enum { AGG_COUNT_STAR = 1, AGG_COUNT, AGG_SUM, AGG_AVG, AGG_MIN, AGG_MAX };
/* constraint kinds */
enum { C_PRIMARY = 1, C_NOTNULL, C_NULL, C_UNIQUE, C_DEFAULT, C_CHECK, C_FOREIGN };
/* ALTER TABLE actions */
enum { A_ADD = 1, A_DROP, A_MODIFY, A_RENAME_COL, A_RENAME_TABLE };
/* join kinds */
enum { J_INNER = 1, J_LEFT, J_RIGHT, J_FULL, J_CROSS };
/* set operations */
enum { S_UNION = 1, S_INTERSECT, S_EXCEPT };
/* SHOW kinds */
enum { SH_TABLES = 1, SH_DATABASES, SH_USERS, SH_GRANTS, SH_VIEWS, SH_INDEXES, SH_TRANSACTION };
/* privileges (bit mask) */
enum { P_SELECT = 1, P_INSERT = 2, P_UPDATE = 4, P_DELETE = 8, P_ALL = 15 };

typedef struct Node {
    NodeType type;
    int op, flag;
    char *s1, *s2, *s3;
    long long ival;
    double fval;
    struct Node **kids;
    int nkids, cap;
    int line, col;

    /* ---- semantic annotations ---- */
    int   dtype;         /* inferred data type                             */
    long long dlen;      /* declared length (VARCHAR(n))                   */
    int   depth;         /* COLREF: how many query scopes outward          */
    char *rqual;         /* COLREF: resolved range name (table or alias)   */
    int   slot;          /* index into aggregated row (-1 = none)          */
    int   correlated;    /* QUERY: references an outer query               */
    int   subq_id;       /* number used when printing IR                   */
    void *info;          /* QInfo (queries) / TableSym (CREATE TABLE) ...  */
    void *plan;          /* compiled IR for sub-queries / derived tables   */

    /* ---- runtime caches ---- */
    int   bind_sid, bind_idx;
    void *cache;
    int   cached;
} Node;

/* Semantic information attached to N_SELECT / N_SETOP / N_QUERY nodes */
typedef struct QInfo {
    int    nout;          /* output columns                                 */
    char **out_names;
    int   *out_types;
    char **out_quals;     /* qualifier of each output (for derived tables)  */
    /* SELECT only */
    Node **items;  int nitems;   /* select list after '*' expansion         */
    int    has_agg;
    Node **gkeys;  int ngkeys;   /* GROUP BY expressions                    */
    Node **aggs;   int naggs;    /* distinct aggregate calls                */
    /* QUERY only: ORDER BY resolution */
    int    nsort;
    int   *sort_col;      /* output column index, or -1 => hidden expression */
    Node **sort_expr;
    int   *sort_desc;
    int    nhidden;
} QInfo;

/* construction */
Node *node_new(NodeType t, int line, int col);
Node *node_add(Node *parent, Node *kid);          /* returns parent */
Node *node_kid(Node *n, int i);                   /* NULL-safe      */
Node *mk_list(void);
Node *mk_ident(const char *name, int line, int col);
Node *mk_binop(int op, Node *a, Node *b);
Node *mk_unop(int op, Node *a);
Node *mk_int(long long v);
Node *mk_float(double v);
Node *mk_string(const char *s);
Node *mk_bool(int v);
Node *mk_null(void);
Node *node_clone(Node *n);

/* printing */
void  ast_print(Node *root);                  /* pretty tree        */
char *expr_to_sql(Node *e);                   /* unparse expression */
char *query_to_sql(Node *q);                  /* unparse query      */
char *datatype_to_sql(int dt, long long len, int scale);
const char *op_symbol(int op);
const char *agg_name(int kind);
const char *join_name(int kind);
const char *setop_name(int kind, int all);
char *priv_mask_str(int mask);
const char *stmt_category(Node *stmt);        /* DDL/DML/DQL/DCL/TCL */
const char *stmt_name(Node *stmt);

extern int g_unparse_subq_ref;   /* print sub-queries as SUBQ#n inside IR */
extern int g_unparse_resolved;   /* print column refs with their resolved range */

#endif
