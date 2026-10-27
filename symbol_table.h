/*
 * symbol_table.h - the compiler's symbol table / system catalog
 *
 * Keeps track of every named object the compiler knows about:
 *
 *     Databases -> Tables -> Columns (type, length, constraints)
 *               -> Views, Indexes
 *               -> Users -> Privileges -> Database objects
 *
 * Tables, views, indexes and users are stored in chained hash tables
 * keyed by (case-insensitive) name. The catalog is persisted inside the
 * database file in a meta table, as canonical SQL that is re-parsed by
 * our own parser on start-up.
 */
#ifndef SYMBOL_TABLE_H
#define SYMBOL_TABLE_H

#include "ast.h"
#include <sqlite3.h>

/* ---------- generic hash table ---------- */
typedef struct HEntry { char *key; void *val; struct HEntry *next; } HEntry;
typedef struct { HEntry **buckets; int nbuckets; int count; } HashTable;

HashTable *ht_new(int nbuckets);
void  ht_put(HashTable *h, const char *key, void *val);
void *ht_get(HashTable *h, const char *key);
int   ht_del(HashTable *h, const char *key);
char **ht_sorted_keys(HashTable *h, int *n);   /* caller frees array */

/* ---------- symbols ---------- */
typedef struct {
    char  name[MAX_NAME];
    int   type;              /* DataType          */
    long long len;           /* VARCHAR(n) etc.   */
    int   scale;
    int   not_null;
    int   primary;           /* part of PRIMARY KEY */
    int   unique;            /* single-column UNIQUE */
    char *default_sql;       /* canonical SQL of DEFAULT expression or NULL */
    Node *default_expr;
} ColumnSym;

typedef struct {
    int    kind;             /* C_PRIMARY or C_UNIQUE */
    char  *name;
    int    ncols;
    char **cols;
} KeySym;

typedef struct {
    char  *name;
    int    ncols;
    char **cols;
    char  *ref_table;
    char **ref_cols;
} FKSym;

typedef struct {
    char *name;
    char *sql;
    Node *expr;
} CheckSym;

typedef struct TableSym {
    char       name[MAX_NAME];
    int        ncols;
    ColumnSym *cols;
    int        nkeys;   KeySym   *keys;
    int        nfks;    FKSym    *fks;
    int        nchecks; CheckSym *checks;
} TableSym;

typedef struct {
    char   name[MAX_NAME];
    char  *sql;              /* the SELECT text */
    Node  *query;            /* parsed on demand */
    int    ncols;
    char **colnames;
    int   *coltypes;
} ViewSym;

typedef struct {
    char   name[MAX_NAME];
    char   table[MAX_NAME];
    int    unique;
    int    ncols;
    char **cols;
} IndexSym;

typedef struct {
    char  object[MAX_NAME];
    int   privs;             /* P_* mask */
} PrivEntry;

typedef struct {
    char       name[MAX_NAME];
    int        is_admin;
    int        npriv;
    PrivEntry *privs;
} UserSym;

typedef struct {
    char       dbname[MAX_NAME];
    HashTable *tables;
    HashTable *views;
    HashTable *indexes;
    HashTable *users;
} Catalog;

extern Catalog g_cat;
extern char    g_current_user[MAX_NAME];
extern int     g_symlog;          /* log symbol-table operations */
extern PtrVec  g_symlog_lines;

void symtab_init(const char *dbname);
void symtab_clear(void);
void symlog(const char *fmt, ...);
void symlog_reset(void);

/* tables & columns */
TableSym  *table_new(const char *name);
void       insert_table(TableSym *t);
TableSym  *lookup_table(const char *name);
void       delete_table(const char *name);
ColumnSym *insert_column(TableSym *t, const char *name, int type, long long len, int scale);
ColumnSym *lookup_column(TableSym *t, const char *name);
int        column_index(TableSym *t, const char *name);
void       table_add_key(TableSym *t, int kind, const char *cname, int ncols, char **cols);
void       table_add_fk(TableSym *t, const char *cname, int ncols, char **cols, const char *rtable, char **rcols);
void       table_add_check(TableSym *t, const char *cname, const char *sql, Node *expr);
KeySym    *table_primary_key(TableSym *t);
char      *table_to_sql(TableSym *t);
char      *column_type_str(ColumnSym *c);
TableSym  *table_copy(TableSym *t);

/* views */
void     insert_view(ViewSym *v);
ViewSym *lookup_view(const char *name);
void     delete_view(const char *name);

/* indexes */
void      insert_index(IndexSym *ix);
IndexSym *lookup_index(const char *name);
void      delete_index(const char *name);

/* users & privileges */
UserSym *insert_user(const char *name);
UserSym *lookup_user(const char *name);
void     delete_user(const char *name);
void     grant_privilege(const char *user, const char *object, int privs);
int      revoke_privilege(const char *user, const char *object, int privs);  /* returns privs actually removed */
int      has_privilege(const char *user, const char *object, int priv);
int      user_privs_on(const char *user, const char *object);
void     rename_object_privileges(const char *old, const char *new_name);
void     drop_object_privileges(const char *object);

/* persistence (meta table __sqlc_catalog inside the database file) */
int  symtab_save(sqlite3 *db);
int  symtab_load(sqlite3 *db);

/* display */
void symtab_dump(void);
void symtab_print_table(TableSym *t);

#endif
