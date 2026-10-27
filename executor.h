/*
 * executor.h - execution layer
 *
 * Interprets the optimized IR. Query operators (LOAD, FILTER, JOIN,
 * GROUP, AGGREGATE, PROJECT, SORT, ...) run in this engine over
 * in-memory relations; SQLite is used purely as the storage engine
 * (reading table rows, writing inserted / updated / deleted rows,
 * physical tables and indexes, durable transactions).
 */
#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "ir.h"
#include "eval.h"
#include "util.h"
#include <sqlite3.h>

extern sqlite3 *g_db;
extern char     g_dbname[MAX_NAME];
extern char     g_dbdir[512];

int   executor_open(const char *dir, const char *dbname);
void  executor_close(void);
int   db_file_exists(const char *name);
int   execute_plan(Plan *p);          /* 0 = success, -1 = execution error */
Relation *exec_node(IRNode *n, Env *outer);
void  print_relation(Relation *r, double secs);

#endif
