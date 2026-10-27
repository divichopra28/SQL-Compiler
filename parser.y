/*
 * parser.y - Bison grammar for SQL-C
 *
 * The grammar is organised by SQL command category:
 *
 *   sql_statement : ddl_statement | dml_statement | dql_statement
 *                 | dcl_statement | tcl_statement | utility_statement
 *
 * Each rule builds AST nodes (see ast.h). Operator precedence for
 * expressions is declared with %left / %right / %nonassoc below:
 *
 *      OR  <  AND  <  NOT  <  comparisons, LIKE, IN, IS  <  + -  <  * / %  <  unary -
 *
 * so  "age > 18 AND cgpa > 8 OR branch = 'CSE'"  parses as
 *     "((age > 18) AND (cgpa > 8)) OR (branch = 'CSE')".
 *
 * BETWEEN uses a restricted operand (b_expr) that cannot contain AND/OR,
 * which removes the classic  "x BETWEEN a AND b AND c"  ambiguity.
 */
%code requires {
#include "ast.h"
}

%code {
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "diag.h"
#include "lexer.h"

int yylex(void);
void yyerror(const char *msg);

Node *g_parse_result = NULL;
int g_parse_report = 1;

#define LOC(n, l) do { (n)->line = (l).first_line; (n)->col = (l).first_column; } while (0)

static Node *mk_node(NodeType t, YYLTYPE l) { return node_new(t, l.first_line, l.first_column); }
static Node *mk_dtype(int dt, long long len, int scale, YYLTYPE l) {
    Node *n = mk_node(N_DATATYPE, l);
    n->op = dt; n->ival = len; n->flag = scale;
    return n;
}
static Node *mk_bin(int op, Node *a, Node *b, YYLTYPE l) {
    Node *n = mk_binop(op, a, b);
    LOC(n, l);
    return n;
}
static Node *mk_agg(int kind, int distinct, Node *arg, YYLTYPE l) {
    Node *n = mk_node(N_AGG, l);
    n->op = kind; n->flag = distinct;
    node_add(n, arg);
    return n;
}
static Node *mk_select(int distinct, Node *items, Node *from, Node *where, Node *group, Node *having, YYLTYPE l) {
    Node *n = mk_node(N_SELECT, l);
    n->flag = distinct;
    node_add(n, items); node_add(n, from); node_add(n, where);
    node_add(n, group); node_add(n, having);
    return n;
}
static Node *mk_named(NodeType t, char *name, int flag, YYLTYPE l) {
    Node *n = mk_node(t, l);
    n->s1 = name; n->flag = flag;
    return n;
}
}

%define parse.error custom
%locations

%union {
    long long ival;
    double    fval;
    char     *sval;
    int       num;
    Node     *node;
}

/* ---------- literals / identifiers ---------- */
%token <ival> INTNUM   "integer literal"
%token <fval> FLOATNUM "float literal"
%token <sval> STRING   "string literal"
%token <sval> DATE_LIT "date literal"
%token <sval> IDENT    "identifier"
%token T_NE "<>"  T_LE "<="  T_GE ">="
%token LEX_ERROR "invalid token"

/* ---------- keywords ---------- */
%token K_ADD "ADD" K_ALL "ALL" K_ALTER "ALTER" K_AND "AND" K_AS "AS" K_ASC "ASC"
%token K_AVG "AVG" K_BEGIN "BEGIN" K_BETWEEN "BETWEEN" K_BIGINT "BIGINT" K_BOOL "BOOL"
%token K_BOOLEAN "BOOLEAN" K_BY "BY" K_CHAR "CHAR" K_CHECK "CHECK" K_COLUMN "COLUMN"
%token K_COMMIT "COMMIT" K_CONNECT "CONNECT" K_CONSTRAINT "CONSTRAINT" K_COUNT "COUNT"
%token K_CREATE "CREATE" K_CROSS "CROSS" K_DATABASE "DATABASE" K_DATABASES "DATABASES"
%token K_DATE "DATE" K_DATETIME "DATETIME" K_DECIMAL "DECIMAL" K_DEFAULT "DEFAULT"
%token K_DELETE "DELETE" K_DESC "DESC" K_DESCRIBE "DESCRIBE" K_DISTINCT "DISTINCT"
%token K_DOUBLE "DOUBLE" K_DROP "DROP" K_EXCEPT "EXCEPT" K_EXISTS "EXISTS"
%token K_EXPLAIN "EXPLAIN" K_FALSE "FALSE" K_FLOAT "FLOAT" K_FOR "FOR" K_FOREIGN "FOREIGN"
%token K_FROM "FROM" K_FULL "FULL" K_GRANT "GRANT" K_GRANTS "GRANTS" K_GROUP "GROUP"
%token K_HAVING "HAVING" K_IF "IF" K_IN "IN" K_INDEX "INDEX" K_INDEXES "INDEXES"
%token K_INNER "INNER" K_INSERT "INSERT" K_INT "INT" K_INTEGER "INTEGER"
%token K_INTERSECT "INTERSECT" K_INTO "INTO" K_IS "IS" K_JOIN "JOIN" K_KEY "KEY"
%token K_LEFT "LEFT" K_LIKE "LIKE" K_LIMIT "LIMIT" K_MAX "MAX" K_MIN "MIN"
%token K_MODIFY "MODIFY" K_NOT "NOT" K_NULL "NULL" K_NUMERIC "NUMERIC" K_OFFSET "OFFSET"
%token K_ON "ON" K_OR "OR" K_ORDER "ORDER" K_OUTER "OUTER" K_PRIMARY "PRIMARY"
%token K_PRIVILEGES "PRIVILEGES" K_REAL "REAL" K_REFERENCES "REFERENCES"
%token K_RELEASE "RELEASE" K_RENAME "RENAME" K_REVOKE "REVOKE" K_RIGHT "RIGHT"
%token K_ROLLBACK "ROLLBACK" K_SAVEPOINT "SAVEPOINT" K_SELECT "SELECT" K_SET "SET"
%token K_SHOW "SHOW" K_SMALLINT "SMALLINT" K_START "START" K_SUM "SUM" K_TABLE "TABLE"
%token K_TABLES "TABLES" K_TEXT "TEXT" K_TIME "TIME" K_TO "TO" K_TRANSACTION "TRANSACTION"
%token K_TRUE "TRUE" K_TRUNCATE "TRUNCATE" K_UNION "UNION" K_UNIQUE "UNIQUE"
%token K_UPDATE "UPDATE" K_USE "USE" K_USER "USER" K_USERS "USERS" K_VALUES "VALUES"
%token K_VARCHAR "VARCHAR" K_VIEW "VIEW" K_VIEWS "VIEWS" K_WHERE "WHERE" K_WORK "WORK"

/* ---------- precedence (lowest first) ---------- */
%left     K_UNION K_EXCEPT
%left     K_INTERSECT
%left     K_OR
%left     K_AND
%right    K_NOT
%nonassoc '=' T_NE '<' '>' T_LE T_GE K_LIKE K_IN K_IS K_BETWEEN
%left     '+' '-'
%left     '*' '/' '%'
%precedence UMINUS

%type <node> sql_statement ddl_statement dml_statement dql_statement dcl_statement
%type <node> tcl_statement utility_statement explainable
%type <node> create_database_stmt drop_database_stmt create_table_stmt create_view_stmt
%type <node> create_index_stmt alter_table_stmt alter_action drop_stmt truncate_stmt
%type <node> table_element_list table_element column_def data_type col_constraint_list
%type <node> col_constraint table_constraint default_value
%type <node> insert_stmt update_stmt delete_stmt opt_column_list values_list
%type <node> assign_list assign
%type <node> query_stmt query_expr select_core select_list select_item
%type <node> opt_from table_expr table_primary opt_where opt_group_by opt_having
%type <node> opt_order_by order_list order_item opt_limit_clause
%type <node> expr b_expr primary literal column_ref function_call expr_list opt_expr_list
%type <node> ident_list priv_list priv user_list
%type <node> grant_stmt revoke_stmt
%type <sval> ident opt_alias opt_table_alias
%type <num>  opt_distinct opt_all join_kind opt_asc_desc opt_if_exists opt_if_not_exists
%type <num>  opt_unique opt_len

%start input

%%

input
    : sql_statement opt_semicolon         { g_parse_result = $1; }
    ;

opt_semicolon
    : %empty
    | ';'
    ;

sql_statement
    : ddl_statement
    | dml_statement
    | dql_statement
    | dcl_statement
    | tcl_statement
    | utility_statement
    ;

/* ======================================================================
 *  DDL - Data Definition Language
 * ====================================================================== */
ddl_statement
    : create_database_stmt
    | drop_database_stmt
    | create_table_stmt
    | create_view_stmt
    | create_index_stmt
    | alter_table_stmt
    | drop_stmt
    | truncate_stmt
    ;

create_database_stmt
    : K_CREATE K_DATABASE opt_if_not_exists ident   { $$ = mk_named(N_CREATE_DATABASE, $4, $3, @1); }
    ;

drop_database_stmt
    : K_DROP K_DATABASE opt_if_exists ident          { $$ = mk_named(N_DROP_DATABASE, $4, $3, @1); }
    ;

opt_if_not_exists
    : %empty                  { $$ = 0; }
    | K_IF K_NOT K_EXISTS     { $$ = 1; }
    ;

opt_if_exists
    : %empty                  { $$ = 0; }
    | K_IF K_EXISTS           { $$ = 1; }
    ;

create_table_stmt
    : K_CREATE K_TABLE opt_if_not_exists ident '(' table_element_list ')'
        {
            $$ = $6;
            $$->type = N_CREATE_TABLE;
            $$->s1 = $4;
            $$->flag = $3;
            LOC($$, @1);
        }
    ;

table_element_list
    : table_element                              { $$ = node_add(mk_list(), $1); }
    | table_element_list ',' table_element       { $$ = node_add($1, $3); }
    ;

table_element
    : column_def
    | table_constraint
    ;

column_def
    : ident data_type col_constraint_list
        {
            $$ = mk_node(N_COLUMN_DEF, @1);
            $$->s1 = $1;
            node_add($$, $2);
            node_add($$, $3);
        }
    ;

data_type
    : K_INT                         { $$ = mk_dtype(DT_INT, 0, 0, @1); }
    | K_INTEGER                     { $$ = mk_dtype(DT_INT, 0, 0, @1); }
    | K_SMALLINT                    { $$ = mk_dtype(DT_SMALLINT, 0, 0, @1); }
    | K_BIGINT                      { $$ = mk_dtype(DT_BIGINT, 0, 0, @1); }
    | K_FLOAT opt_len               { $$ = mk_dtype(DT_FLOAT, 0, 0, @1); }
    | K_REAL                        { $$ = mk_dtype(DT_FLOAT, 0, 0, @1); }
    | K_DOUBLE                      { $$ = mk_dtype(DT_DOUBLE, 0, 0, @1); }
    | K_DECIMAL                     { $$ = mk_dtype(DT_DECIMAL, 0, 0, @1); }
    | K_DECIMAL '(' INTNUM ')'      { $$ = mk_dtype(DT_DECIMAL, $3, 0, @1); }
    | K_DECIMAL '(' INTNUM ',' INTNUM ')' { $$ = mk_dtype(DT_DECIMAL, $3, (int)$5, @1); }
    | K_NUMERIC                     { $$ = mk_dtype(DT_NUMERIC, 0, 0, @1); }
    | K_NUMERIC '(' INTNUM ')'      { $$ = mk_dtype(DT_NUMERIC, $3, 0, @1); }
    | K_NUMERIC '(' INTNUM ',' INTNUM ')' { $$ = mk_dtype(DT_NUMERIC, $3, (int)$5, @1); }
    | K_CHAR opt_len                { $$ = mk_dtype(DT_CHAR, $2 ? $2 : 1, 0, @1); }
    | K_VARCHAR opt_len             { $$ = mk_dtype(DT_VARCHAR, $2 ? $2 : 255, 0, @1); }
    | K_TEXT                        { $$ = mk_dtype(DT_TEXT, 0, 0, @1); }
    | K_DATE                        { $$ = mk_dtype(DT_DATE, 0, 0, @1); }
    | K_TIME                        { $$ = mk_dtype(DT_TIME, 0, 0, @1); }
    | K_DATETIME                    { $$ = mk_dtype(DT_DATETIME, 0, 0, @1); }
    | K_BOOLEAN                     { $$ = mk_dtype(DT_BOOLEAN, 0, 0, @1); }
    | K_BOOL                        { $$ = mk_dtype(DT_BOOLEAN, 0, 0, @1); }
    ;

opt_len
    : %empty                        { $$ = 0; }
    | '(' INTNUM ')'                { $$ = (int)$2; }
    ;

col_constraint_list
    : %empty                                  { $$ = mk_list(); }
    | col_constraint_list col_constraint      { $$ = node_add($1, $2); }
    ;

col_constraint
    : K_PRIMARY K_KEY               { $$ = mk_node(N_COL_CONSTRAINT, @1); $$->op = C_PRIMARY; }
    | K_NOT K_NULL                  { $$ = mk_node(N_COL_CONSTRAINT, @1); $$->op = C_NOTNULL; }
    | K_NULL                        { $$ = mk_node(N_COL_CONSTRAINT, @1); $$->op = C_NULL; }
    | K_UNIQUE                      { $$ = mk_node(N_COL_CONSTRAINT, @1); $$->op = C_UNIQUE; }
    | K_DEFAULT default_value       { $$ = mk_node(N_COL_CONSTRAINT, @1); $$->op = C_DEFAULT; node_add($$, $2); }
    | K_CHECK '(' expr ')'          { $$ = mk_node(N_COL_CONSTRAINT, @1); $$->op = C_CHECK; node_add($$, $3); }
    | K_REFERENCES ident '(' ident ')'
        { $$ = mk_node(N_COL_CONSTRAINT, @1); $$->op = C_FOREIGN; $$->s1 = $2; $$->s2 = $4; }
    | K_CONSTRAINT ident col_constraint { $$ = $3; $$->s3 = $2; }
    ;

default_value
    : literal
    | '-' INTNUM                    { $$ = mk_int(-$2); LOC($$, @1); }
    | '-' FLOATNUM                  { $$ = mk_float(-$2); LOC($$, @1); }
    ;

table_constraint
    : K_PRIMARY K_KEY '(' ident_list ')'
        { $$ = mk_node(N_TABLE_CONSTRAINT, @1); $$->op = C_PRIMARY; node_add($$, $4); node_add($$, NULL); node_add($$, NULL); }
    | K_UNIQUE '(' ident_list ')'
        { $$ = mk_node(N_TABLE_CONSTRAINT, @1); $$->op = C_UNIQUE; node_add($$, $3); node_add($$, NULL); node_add($$, NULL); }
    | K_FOREIGN K_KEY '(' ident_list ')' K_REFERENCES ident '(' ident_list ')'
        { $$ = mk_node(N_TABLE_CONSTRAINT, @1); $$->op = C_FOREIGN; $$->s1 = $7;
          node_add($$, $4); node_add($$, $9); node_add($$, NULL); }
    | K_CHECK '(' expr ')'
        { $$ = mk_node(N_TABLE_CONSTRAINT, @1); $$->op = C_CHECK; node_add($$, NULL); node_add($$, NULL); node_add($$, $3); }
    | K_CONSTRAINT ident table_constraint     { $$ = $3; $$->s3 = $2; }
    ;

create_view_stmt
    : K_CREATE K_VIEW ident K_AS query_stmt
        { $$ = mk_node(N_CREATE_VIEW, @1); $$->s1 = $3; node_add($$, $5); }
    ;

create_index_stmt
    : K_CREATE opt_unique K_INDEX ident K_ON ident '(' ident_list ')'
        { $$ = mk_node(N_CREATE_INDEX, @1); $$->flag = $2; $$->s1 = $4; $$->s2 = $6; node_add($$, $8); }
    ;

opt_unique
    : %empty          { $$ = 0; }
    | K_UNIQUE        { $$ = 1; }
    ;

alter_table_stmt
    : K_ALTER K_TABLE ident alter_action
        { $$ = mk_node(N_ALTER_TABLE, @1); $$->s1 = $3; node_add($$, $4); }
    ;

alter_action
    : K_ADD column_def                 { $$ = mk_node(N_ALTER_ACTION, @1); $$->op = A_ADD; node_add($$, $2); }
    | K_ADD K_COLUMN column_def        { $$ = mk_node(N_ALTER_ACTION, @1); $$->op = A_ADD; node_add($$, $3); }
    | K_DROP K_COLUMN ident            { $$ = mk_node(N_ALTER_ACTION, @1); $$->op = A_DROP; $$->s1 = $3; }
    | K_DROP ident                     { $$ = mk_node(N_ALTER_ACTION, @1); $$->op = A_DROP; $$->s1 = $2; }
    | K_MODIFY column_def              { $$ = mk_node(N_ALTER_ACTION, @1); $$->op = A_MODIFY; node_add($$, $2); }
    | K_MODIFY K_COLUMN column_def     { $$ = mk_node(N_ALTER_ACTION, @1); $$->op = A_MODIFY; node_add($$, $3); }
    | K_RENAME K_COLUMN ident K_TO ident
        { $$ = mk_node(N_ALTER_ACTION, @1); $$->op = A_RENAME_COL; $$->s1 = $3; $$->s2 = $5; }
    | K_RENAME K_TO ident              { $$ = mk_node(N_ALTER_ACTION, @1); $$->op = A_RENAME_TABLE; $$->s2 = $3; }
    ;

drop_stmt
    : K_DROP K_TABLE opt_if_exists ident    { $$ = mk_named(N_DROP_TABLE, $4, $3, @1); }
    | K_DROP K_VIEW opt_if_exists ident     { $$ = mk_named(N_DROP_VIEW, $4, $3, @1); }
    | K_DROP K_INDEX opt_if_exists ident    { $$ = mk_named(N_DROP_INDEX, $4, $3, @1); }
    ;

truncate_stmt
    : K_TRUNCATE K_TABLE ident          { $$ = mk_named(N_TRUNCATE, $3, 0, @1); }
    | K_TRUNCATE ident                  { $$ = mk_named(N_TRUNCATE, $2, 0, @1); }
    ;

/* ======================================================================
 *  DML - Data Manipulation Language
 * ====================================================================== */
dml_statement
    : insert_stmt
    | update_stmt
    | delete_stmt
    ;

insert_stmt
    : K_INSERT K_INTO ident opt_column_list K_VALUES values_list
        { $$ = mk_node(N_INSERT, @1); $$->s1 = $3; node_add($$, $4); node_add($$, $6); }
    | K_INSERT K_INTO ident opt_column_list query_stmt
        { $$ = mk_node(N_INSERT, @1); $$->s1 = $3; node_add($$, $4); node_add($$, $5); }
    ;

opt_column_list
    : %empty                          { $$ = NULL; }
    | '(' ident_list ')'              { $$ = $2; }
    ;

values_list
    : '(' expr_list ')'                   { $$ = node_add(mk_list(), $2); LOC($2, @1); }
    | values_list ',' '(' expr_list ')'   { $$ = node_add($1, $4); LOC($4, @3); }
    ;

update_stmt
    : K_UPDATE ident K_SET assign_list opt_where
        { $$ = mk_node(N_UPDATE, @1); $$->s1 = $2; node_add($$, $4); node_add($$, $5); }
    ;

assign_list
    : assign                          { $$ = node_add(mk_list(), $1); }
    | assign_list ',' assign          { $$ = node_add($1, $3); }
    ;

assign
    : ident '=' expr                  { $$ = mk_node(N_ASSIGN, @1); $$->s1 = $1; node_add($$, $3); }
    ;

delete_stmt
    : K_DELETE K_FROM ident opt_where
        { $$ = mk_node(N_DELETE, @1); $$->s1 = $3; node_add($$, $4); }
    ;

/* ======================================================================
 *  DQL - Data Query Language (SELECT)
 * ====================================================================== */
dql_statement
    : query_stmt
    ;

query_stmt
    : query_expr opt_order_by opt_limit_clause
        {
            $$ = mk_node(N_QUERY, @1);
            node_add($$, $1);
            node_add($$, $2);
            node_add($$, $3 ? $3->kids[0] : NULL);
            node_add($$, $3 ? $3->kids[1] : NULL);
        }
    ;

query_expr
    : select_core
    | query_expr K_UNION opt_all query_expr
        { $$ = mk_node(N_SETOP, @2); $$->op = S_UNION; $$->flag = $3; node_add($$, $1); node_add($$, $4); }
    | query_expr K_INTERSECT opt_all query_expr
        { $$ = mk_node(N_SETOP, @2); $$->op = S_INTERSECT; $$->flag = $3; node_add($$, $1); node_add($$, $4); }
    | query_expr K_EXCEPT opt_all query_expr
        { $$ = mk_node(N_SETOP, @2); $$->op = S_EXCEPT; $$->flag = $3; node_add($$, $1); node_add($$, $4); }
    ;

opt_all
    : %empty       { $$ = 0; }
    | K_ALL        { $$ = 1; }
    ;

select_core
    : K_SELECT opt_distinct select_list opt_from opt_where opt_group_by opt_having
        { $$ = mk_select($2, $3, $4, $5, $6, $7, @1); }
    ;

opt_distinct
    : %empty       { $$ = 0; }
    | K_DISTINCT   { $$ = 1; }
    | K_ALL        { $$ = 0; }
    ;

select_list
    : select_item                    { $$ = node_add(mk_list(), $1); }
    | select_list ',' select_item    { $$ = node_add($1, $3); }
    ;

select_item
    : '*'                            { $$ = mk_node(N_STAR, @1); }
    | ident '.' '*'                  { $$ = mk_node(N_STAR, @1); $$->s1 = $1; }
    | expr opt_alias                 { $$ = mk_node(N_SELECT_ITEM, @1); node_add($$, $1); $$->s1 = $2; }
    ;

opt_alias
    : %empty                         { $$ = NULL; }
    | K_AS ident                     { $$ = $2; }
    | ident                          { $$ = $1; }
    ;

opt_from
    : %empty                         { $$ = NULL; }
    | K_FROM table_expr              { $$ = $2; }
    ;

table_expr
    : table_primary
    | table_expr join_kind K_JOIN table_primary K_ON expr
        { $$ = mk_node(N_JOIN, @3); $$->op = $2; node_add($$, $1); node_add($$, $4); node_add($$, $6); }
    | table_expr K_CROSS K_JOIN table_primary
        { $$ = mk_node(N_JOIN, @2); $$->op = J_CROSS; node_add($$, $1); node_add($$, $4); node_add($$, NULL); }
    | table_expr ',' table_primary
        { $$ = mk_node(N_JOIN, @2); $$->op = J_CROSS; $$->flag = 1; node_add($$, $1); node_add($$, $3); node_add($$, NULL); }
    ;

join_kind
    : %empty                 { $$ = J_INNER; }
    | K_INNER                { $$ = J_INNER; }
    | K_LEFT                 { $$ = J_LEFT; }
    | K_LEFT K_OUTER         { $$ = J_LEFT; }
    | K_RIGHT                { $$ = J_RIGHT; }
    | K_RIGHT K_OUTER        { $$ = J_RIGHT; }
    | K_FULL                 { $$ = J_FULL; }
    | K_FULL K_OUTER         { $$ = J_FULL; }
    ;

table_primary
    : ident opt_table_alias
        { $$ = mk_node(N_TABLE_REF, @1); $$->s1 = $1; $$->s2 = $2; }
    | '(' query_stmt ')' opt_table_alias
        { $$ = mk_node(N_DERIVED_TABLE, @1); node_add($$, $2); $$->s2 = $4; }
    ;

opt_table_alias
    : %empty                 { $$ = NULL; }
    | K_AS ident             { $$ = $2; }
    | ident                  { $$ = $1; }
    ;

opt_where
    : %empty                 { $$ = NULL; }
    | K_WHERE expr           { $$ = $2; }
    ;

opt_group_by
    : %empty                 { $$ = NULL; }
    | K_GROUP K_BY expr_list { $$ = $3; }
    ;

opt_having
    : %empty                 { $$ = NULL; }
    | K_HAVING expr          { $$ = $2; }
    ;

opt_order_by
    : %empty                 { $$ = NULL; }
    | K_ORDER K_BY order_list { $$ = $3; }
    ;

order_list
    : order_item                  { $$ = node_add(mk_list(), $1); }
    | order_list ',' order_item   { $$ = node_add($1, $3); }
    ;

order_item
    : expr opt_asc_desc      { $$ = mk_node(N_ORDER_ITEM, @1); node_add($$, $1); $$->flag = $2; }
    ;

opt_asc_desc
    : %empty                 { $$ = 0; }
    | K_ASC                  { $$ = 0; }
    | K_DESC                 { $$ = 1; }
    ;

opt_limit_clause
    : %empty                 { $$ = NULL; }
    | K_LIMIT INTNUM
        { $$ = mk_list(); node_add($$, mk_int($2)); node_add($$, NULL); }
    | K_LIMIT INTNUM K_OFFSET INTNUM
        { $$ = mk_list(); node_add($$, mk_int($2)); node_add($$, mk_int($4)); }
    | K_LIMIT INTNUM ',' INTNUM
        { $$ = mk_list(); node_add($$, mk_int($4)); node_add($$, mk_int($2)); }
    ;

/* ======================================================================
 *  Expressions
 * ====================================================================== */
expr
    : expr K_OR expr                  { $$ = mk_bin(O_OR, $1, $3, @2); }
    | expr K_AND expr                 { $$ = mk_bin(O_AND, $1, $3, @2); }
    | K_NOT expr                      { $$ = mk_unop(O_NOT, $2); LOC($$, @1); }
    | expr '=' expr                   { $$ = mk_bin(O_EQ, $1, $3, @2); }
    | expr T_NE expr                  { $$ = mk_bin(O_NE, $1, $3, @2); }
    | expr '<' expr                   { $$ = mk_bin(O_LT, $1, $3, @2); }
    | expr '>' expr                   { $$ = mk_bin(O_GT, $1, $3, @2); }
    | expr T_LE expr                  { $$ = mk_bin(O_LE, $1, $3, @2); }
    | expr T_GE expr                  { $$ = mk_bin(O_GE, $1, $3, @2); }
    | expr K_BETWEEN b_expr K_AND b_expr
        { $$ = mk_node(N_BETWEEN, @2); node_add($$, $1); node_add($$, $3); node_add($$, $5); }
    | expr K_NOT K_BETWEEN b_expr K_AND b_expr %prec K_BETWEEN
        { $$ = mk_node(N_BETWEEN, @2); $$->flag = 1; node_add($$, $1); node_add($$, $4); node_add($$, $6); }
    | expr K_IN '(' expr_list ')'
        { $$ = mk_node(N_IN_LIST, @2); node_add($$, $1); node_add($$, $4); }
    | expr K_NOT K_IN '(' expr_list ')' %prec K_IN
        { $$ = mk_node(N_IN_LIST, @2); $$->flag = 1; node_add($$, $1); node_add($$, $5); }
    | expr K_IN '(' query_stmt ')'
        { $$ = mk_node(N_IN_SUBQ, @2); node_add($$, $1); node_add($$, $4); }
    | expr K_NOT K_IN '(' query_stmt ')' %prec K_IN
        { $$ = mk_node(N_IN_SUBQ, @2); $$->flag = 1; node_add($$, $1); node_add($$, $5); }
    | expr K_LIKE expr
        { $$ = mk_node(N_LIKE, @2); node_add($$, $1); node_add($$, $3); }
    | expr K_NOT K_LIKE expr %prec K_LIKE
        { $$ = mk_node(N_LIKE, @2); $$->flag = 1; node_add($$, $1); node_add($$, $4); }
    | expr K_IS K_NULL
        { $$ = mk_node(N_IS_NULL, @2); node_add($$, $1); }
    | expr K_IS K_NOT K_NULL
        { $$ = mk_node(N_IS_NULL, @2); $$->flag = 1; node_add($$, $1); }
    | K_EXISTS '(' query_stmt ')'
        { $$ = mk_node(N_EXISTS, @1); node_add($$, $3); }
    | b_expr
    ;

b_expr
    : b_expr '+' b_expr               { $$ = mk_bin(O_ADD, $1, $3, @2); }
    | b_expr '-' b_expr               { $$ = mk_bin(O_SUB, $1, $3, @2); }
    | b_expr '*' b_expr               { $$ = mk_bin(O_MUL, $1, $3, @2); }
    | b_expr '/' b_expr               { $$ = mk_bin(O_DIV, $1, $3, @2); }
    | b_expr '%' b_expr               { $$ = mk_bin(O_MOD, $1, $3, @2); }
    | '-' b_expr %prec UMINUS         { $$ = mk_unop(O_NEG, $2); LOC($$, @1); }
    | '+' b_expr %prec UMINUS         { $$ = mk_unop(O_POS, $2); LOC($$, @1); }
    | primary
    ;

primary
    : literal
    | column_ref
    | function_call
    | '(' expr ')'                    { $$ = $2; }
    | '(' query_stmt ')'              { $$ = mk_node(N_SUBQUERY, @1); node_add($$, $2); }
    ;

literal
    : INTNUM                          { $$ = mk_int($1); LOC($$, @1); }
    | FLOATNUM                        { $$ = mk_float($1); LOC($$, @1); }
    | STRING                          { $$ = mk_string($1); LOC($$, @1); free($1); }
    | DATE_LIT                        { $$ = mk_node(N_DATE, @1); $$->s1 = $1; $$->dtype = DT_DATE; }
    | K_NULL                          { $$ = mk_null(); LOC($$, @1); }
    | K_TRUE                          { $$ = mk_bool(1); LOC($$, @1); }
    | K_FALSE                         { $$ = mk_bool(0); LOC($$, @1); }
    ;

column_ref
    : ident                           { $$ = mk_node(N_COLREF, @1); $$->s2 = $1; }
    | ident '.' ident                 { $$ = mk_node(N_COLREF, @1); $$->s1 = $1; $$->s2 = $3; }
    ;

function_call
    : K_COUNT '(' '*' ')'                    { $$ = mk_agg(AGG_COUNT_STAR, 0, NULL, @1); }
    | K_COUNT '(' opt_distinct expr ')'      { $$ = mk_agg(AGG_COUNT, $3, $4, @1); }
    | K_SUM '(' opt_distinct expr ')'        { $$ = mk_agg(AGG_SUM, $3, $4, @1); }
    | K_AVG '(' opt_distinct expr ')'        { $$ = mk_agg(AGG_AVG, $3, $4, @1); }
    | K_MIN '(' opt_distinct expr ')'        { $$ = mk_agg(AGG_MIN, $3, $4, @1); }
    | K_MAX '(' opt_distinct expr ')'        { $$ = mk_agg(AGG_MAX, $3, $4, @1); }
    | IDENT '(' opt_expr_list ')'
        { $$ = mk_node(N_FUNC, @1); $$->s1 = str_upper_dup($1); free($1);
          for (int i = 0; $3 && i < $3->nkids; i++) node_add($$, $3->kids[i]); }
    ;

opt_expr_list
    : %empty                          { $$ = NULL; }
    | expr_list
    ;

expr_list
    : expr                            { $$ = node_add(mk_list(), $1); }
    | expr_list ',' expr              { $$ = node_add($1, $3); }
    ;

ident_list
    : ident                           { $$ = node_add(mk_list(), mk_ident($1, @1.first_line, @1.first_column)); }
    | ident_list ',' ident            { $$ = node_add($1, mk_ident($3, @3.first_line, @3.first_column)); }
    ;

/* Identifiers. A few non-reserved keywords may also be used as names
 * (e.g. a column called "date" or "text"). */
ident
    : IDENT
    | K_DATE        { $$ = xstrdup("date"); }
    | K_TIME        { $$ = xstrdup("time"); }
    | K_TEXT        { $$ = xstrdup("text"); }
    | K_KEY         { $$ = xstrdup("key"); }
    | K_USER        { $$ = xstrdup("user"); }
    | K_USERS       { $$ = xstrdup("users"); }
    | K_TABLES      { $$ = xstrdup("tables"); }
    | K_VIEWS       { $$ = xstrdup("views"); }
    | K_DATABASES   { $$ = xstrdup("databases"); }
    | K_GRANTS      { $$ = xstrdup("grants"); }
    | K_INDEXES     { $$ = xstrdup("indexes"); }
    | K_WORK        { $$ = xstrdup("work"); }
    | K_PRIVILEGES  { $$ = xstrdup("privileges"); }
    ;

/* ======================================================================
 *  DCL - Data Control Language
 * ====================================================================== */
dcl_statement
    : grant_stmt
    | revoke_stmt
    | K_CREATE K_USER ident         { $$ = mk_named(N_CREATE_USER, $3, 0, @1); }
    | K_DROP K_USER ident           { $$ = mk_named(N_DROP_USER, $3, 0, @1); }
    | K_CONNECT ident               { $$ = mk_named(N_CONNECT, $2, 0, @1); }
    | K_CONNECT K_TO ident          { $$ = mk_named(N_CONNECT, $3, 0, @1); }
    ;

grant_stmt
    : K_GRANT priv_list K_ON opt_table_kw ident K_TO user_list
        { $$ = mk_node(N_GRANT, @1); node_add($$, $2); $$->s1 = $5; node_add($$, $7); }
    ;

revoke_stmt
    : K_REVOKE priv_list K_ON opt_table_kw ident K_FROM user_list
        { $$ = mk_node(N_REVOKE, @1); node_add($$, $2); $$->s1 = $5; node_add($$, $7); }
    ;

opt_table_kw
    : %empty
    | K_TABLE
    ;

priv_list
    : priv                          { $$ = node_add(mk_list(), $1); }
    | priv_list ',' priv            { $$ = node_add($1, $3); }
    ;

priv
    : K_SELECT                      { $$ = mk_node(N_PRIV, @1); $$->op = P_SELECT; }
    | K_INSERT                      { $$ = mk_node(N_PRIV, @1); $$->op = P_INSERT; }
    | K_UPDATE                      { $$ = mk_node(N_PRIV, @1); $$->op = P_UPDATE; }
    | K_DELETE                      { $$ = mk_node(N_PRIV, @1); $$->op = P_DELETE; }
    | K_ALL                         { $$ = mk_node(N_PRIV, @1); $$->op = P_ALL; }
    | K_ALL K_PRIVILEGES            { $$ = mk_node(N_PRIV, @1); $$->op = P_ALL; }
    ;

user_list
    : ident_list
    ;

/* ======================================================================
 *  TCL - Transaction Control Language
 * ====================================================================== */
tcl_statement
    : K_START K_TRANSACTION                    { $$ = mk_node(N_BEGIN, @1); }
    | K_BEGIN                                  { $$ = mk_node(N_BEGIN, @1); }
    | K_BEGIN K_TRANSACTION                    { $$ = mk_node(N_BEGIN, @1); }
    | K_COMMIT opt_work                        { $$ = mk_node(N_COMMIT, @1); }
    | K_ROLLBACK opt_work                      { $$ = mk_node(N_ROLLBACK, @1); }
    | K_ROLLBACK opt_work K_TO opt_savepoint_kw ident { $$ = mk_named(N_ROLLBACK_TO, $5, 0, @1); }
    | K_SAVEPOINT ident                        { $$ = mk_named(N_SAVEPOINT, $2, 0, @1); }
    | K_RELEASE opt_savepoint_kw ident         { $$ = mk_named(N_RELEASE, $3, 0, @1); }
    ;

opt_work
    : %empty
    | K_WORK
    | K_TRANSACTION
    ;

opt_savepoint_kw
    : %empty
    | K_SAVEPOINT
    ;

/* ======================================================================
 *  Utility statements (not part of the five categories)
 * ====================================================================== */
utility_statement
    : K_USE ident                   { $$ = mk_named(N_USE, $2, 0, @1); }
    | K_SHOW K_TABLES               { $$ = mk_node(N_SHOW, @1); $$->op = SH_TABLES; }
    | K_SHOW K_DATABASES            { $$ = mk_node(N_SHOW, @1); $$->op = SH_DATABASES; }
    | K_SHOW K_USERS                { $$ = mk_node(N_SHOW, @1); $$->op = SH_USERS; }
    | K_SHOW K_VIEWS                { $$ = mk_node(N_SHOW, @1); $$->op = SH_VIEWS; }
    | K_SHOW K_INDEXES              { $$ = mk_node(N_SHOW, @1); $$->op = SH_INDEXES; }
    | K_SHOW K_TRANSACTION          { $$ = mk_node(N_SHOW, @1); $$->op = SH_TRANSACTION; }
    | K_SHOW K_GRANTS               { $$ = mk_node(N_SHOW, @1); $$->op = SH_GRANTS; }
    | K_SHOW K_GRANTS K_FOR ident   { $$ = mk_node(N_SHOW, @1); $$->op = SH_GRANTS; $$->s1 = $4; }
    | K_DESCRIBE ident              { $$ = mk_named(N_DESCRIBE, $2, 0, @1); }
    | K_DESC ident                  { $$ = mk_named(N_DESCRIBE, $2, 0, @1); }
    | K_EXPLAIN explainable         { $$ = mk_node(N_EXPLAIN, @1); node_add($$, $2); }
    ;

explainable
    : query_stmt
    | insert_stmt
    | update_stmt
    | delete_stmt
    ;

%%

/* ----------------------------------------------------------------------
 *  Error reporting: SYNTAX ERROR with the unexpected token and what the
 *  grammar expected at that point.
 * ---------------------------------------------------------------------- */
static const char *pretty_symbol(yysymbol_kind_t k, char *buf, size_t n) {
    const char *name = yysymbol_name(k);
    if (k == YYSYMBOL_YYEOF) return "end of statement";
    if (name[0] == '"') {                    /* "SELECT" -> SELECT */
        size_t len = strlen(name);
        snprintf(buf, n, "%.*s", (int)(len - 2), name + 1);
        return buf;
    }
    return name;
}

static int yyreport_syntax_error(const yypcontext_t *ctx) {
    if (!g_parse_report) return 0;
    yysymbol_kind_t tok = yypcontext_token(ctx);
    const YYLTYPE *loc = yypcontext_location(ctx);
    char b1[64], b2[64];
    char msg[1024];
    int off = 0;

    if (tok == YYSYMBOL_YYEOF)
        off += snprintf(msg + off, sizeof msg - off, "unexpected end of statement");
    else if (tok == YYSYMBOL_LEX_ERROR)
        off += snprintf(msg + off, sizeof msg - off, "invalid token");
    else {
        const char *lexeme = lex_last_text();
        const char *cls = lex_token_class(lex_last_token());
        char lc[32]; int i = 0;
        for (; cls[i] && i < 31; i++) lc[i] = (char)(cls[i] >= 'A' && cls[i] <= 'Z' ? cls[i] + 32 : cls[i]);
        lc[i] = 0;
        off += snprintf(msg + off, sizeof msg - off, "unexpected %s '%s'", lc, lexeme);
    }

    yysymbol_kind_t exp[128];
    int n = yypcontext_expected_tokens(ctx, exp, 128);
    if (n > 0 && n <= 8) {
        off += snprintf(msg + off, sizeof msg - off, "; expected ");
        for (int i = 0; i < n; i++) {
            const char *nm = pretty_symbol(exp[i], b1, sizeof b1);
            off += snprintf(msg + off, sizeof msg - off, "%s%s", i ? (i == n - 1 ? " or " : ", ") : "", nm);
        }
    } else if (n > 8) {
        off += snprintf(msg + off, sizeof msg - off, "; expected one of %d possible tokens (e.g. ", n);
        for (int i = 0; i < 5; i++) {
            const char *nm = pretty_symbol(exp[i], b1, sizeof b1);
            off += snprintf(msg + off, sizeof msg - off, "%s%s", i ? ", " : "", nm);
        }
        off += snprintf(msg + off, sizeof msg - off, ", ...)");
    }
    (void)b2;

    /* friendly hints for common mistakes */
    const char *hint = NULL;
    if (tok == YYSYMBOL_K_FROM) hint = "SELECT needs a column list or '*' before FROM";
    else if (tok == YYSYMBOL_YYEOF || lex_last_token() == ';') hint = "the statement looks incomplete";
    else if (tok == YYSYMBOL_K_WHERE) hint = "check the clause before WHERE";

    diag_error(ERR_SYNTAX, loc->first_line, loc->first_column, "%s", msg);
    if (hint) printf("    hint: %s\n", hint);
    return 0;
}

void yyerror(const char *msg) {
    if (!g_parse_report) return;
    diag_error(ERR_SYNTAX, yylloc.first_line, yylloc.first_column, "%s", msg);
}

const char *parser_symbol_name(int kind) { return yysymbol_name((yysymbol_kind_t)kind); }

/* Parse a single statement from a string (used for the persisted catalog,
 * view definitions and CHECK constraints). Returns NULL on error. */
Node *parse_string(const char *sql, int report) {
    int saved = g_lex_report, saved_p = g_parse_report;
    g_lex_report = report;
    g_parse_report = report;
    g_parse_result = NULL;
    lex_begin(sql, 1);
    int rc = yyparse();
    lex_end();
    g_lex_report = saved;
    g_parse_report = saved_p;
    return rc == 0 ? g_parse_result : NULL;
}
