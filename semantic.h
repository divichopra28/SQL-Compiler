/*
 * semantic.h - semantic analysis
 *
 * Walks the AST after parsing and checks everything the grammar cannot:
 * existence of tables / columns / users, name resolution and scoping
 * (aliases, correlated sub-queries), type compatibility of operators,
 * aggregate / GROUP BY rules, constraint definitions, constant constraint
 * violations and privileges. The AST is decorated with the results
 * (types, resolved qualifiers, aggregate slots, expanded '*').
 */
#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "ast.h"
#include "symbol_table.h"

extern int g_sem_verbose;      /* print the analysis log */

int       sem_analyze(Node *stmt);                 /* returns number of errors */
TableSym *sem_build_table(Node *create, int report);
Node     *sem_view_query(ViewSym *v, int report);  /* parse + analyse a view */
int       sem_view_columns(ViewSym *v);
char     *canon_sql(Node *e);                      /* expression text with resolved names */

#endif
