/*
 * symbol_table.c - symbol table / catalog implementation
 */
#include "symbol_table.h"
#include "semantic.h"
#include "lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

Catalog g_cat;
char    g_current_user[MAX_NAME] = "admin";
int     g_symlog = 0;
PtrVec  g_symlog_lines;

/* ======================================================================
 *  Hash table (separate chaining, case-insensitive keys)
 * ====================================================================== */
static unsigned hash_ci(const char *s) {
    unsigned h = 5381;
    for (; *s; s++) h = h * 33 + (unsigned)tolower((unsigned char)*s);
    return h;
}
HashTable *ht_new(int nb) {
    HashTable *h = xcalloc(1, sizeof(HashTable));
    h->nbuckets = nb;
    h->buckets = xcalloc((size_t)nb, sizeof(HEntry *));
    return h;
}
void ht_put(HashTable *h, const char *key, void *val) {
    unsigned b = hash_ci(key) % (unsigned)h->nbuckets;
    for (HEntry *e = h->buckets[b]; e; e = e->next)
        if (ci_eq(e->key, key)) { e->val = val; return; }
    HEntry *e = xmalloc(sizeof(HEntry));
    e->key = xstrdup(key);
    e->val = val;
    e->next = h->buckets[b];
    h->buckets[b] = e;
    h->count++;
}
void *ht_get(HashTable *h, const char *key) {
    if (!key) return NULL;
    unsigned b = hash_ci(key) % (unsigned)h->nbuckets;
    for (HEntry *e = h->buckets[b]; e; e = e->next)
        if (ci_eq(e->key, key)) return e->val;
    return NULL;
}
int ht_del(HashTable *h, const char *key) {
    unsigned b = hash_ci(key) % (unsigned)h->nbuckets;
    for (HEntry **pp = &h->buckets[b]; *pp; pp = &(*pp)->next) {
        if (ci_eq((*pp)->key, key)) {
            HEntry *e = *pp;
            *pp = e->next;
            free(e->key);
            free(e);
            h->count--;
            return 1;
        }
    }
    return 0;
}
static int cmp_str_ci(const void *a, const void *b) { return ci_cmp(*(char *const *)a, *(char *const *)b); }
char **ht_sorted_keys(HashTable *h, int *n) {
    char **keys = xmalloc(sizeof(char *) * (size_t)(h->count + 1));
    int k = 0;
    for (int i = 0; i < h->nbuckets; i++)
        for (HEntry *e = h->buckets[i]; e; e = e->next) keys[k++] = e->key;
    qsort(keys, (size_t)k, sizeof(char *), cmp_str_ci);
    *n = k;
    return keys;
}

/* ======================================================================
 *  Logging of symbol-table operations (shown in the SYMBOL TABLE phase)
 * ====================================================================== */
void symlog(const char *fmt, ...) {
    if (!g_symlog) return;
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (!pv_contains_str(&g_symlog_lines, buf)) pv_push(&g_symlog_lines, xstrdup(buf));
}
void symlog_reset(void) {
    for (int i = 0; i < g_symlog_lines.n; i++) free(g_symlog_lines.items[i]);
    g_symlog_lines.n = 0;
}

/* ======================================================================
 *  Catalog lifetime
 * ====================================================================== */
void symtab_init(const char *dbname) {
    snprintf(g_cat.dbname, MAX_NAME, "%s", dbname);
    g_cat.tables = ht_new(64);
    g_cat.views = ht_new(32);
    g_cat.indexes = ht_new(32);
    g_cat.users = ht_new(16);
    UserSym *a = insert_user("admin");
    a->is_admin = 1;
}
void symtab_clear(void) {
    /* the old objects are simply dropped (small, short-lived program) */
    char name[MAX_NAME];
    snprintf(name, sizeof name, "%s", g_cat.dbname);
    symtab_init(name);
}

/* ======================================================================
 *  Tables and columns
 * ====================================================================== */
TableSym *table_new(const char *name) {
    TableSym *t = xcalloc(1, sizeof(TableSym));
    snprintf(t->name, MAX_NAME, "%s", name);
    return t;
}
void insert_table(TableSym *t) {
    ht_put(g_cat.tables, t->name, t);
    symlog("insert_table(%s) -> %d column(s)", t->name, t->ncols);
}
TableSym *lookup_table(const char *name) {
    TableSym *t = ht_get(g_cat.tables, name);
    symlog("lookup_table(%s) -> %s", name, t ? "FOUND" : "NOT FOUND");
    return t;
}
void delete_table(const char *name) {
    ht_del(g_cat.tables, name);
    symlog("delete_table(%s)", name);
}
ColumnSym *insert_column(TableSym *t, const char *name, int type, long long len, int scale) {
    t->cols = xrealloc(t->cols, sizeof(ColumnSym) * (size_t)(t->ncols + 1));
    ColumnSym *c = &t->cols[t->ncols++];
    memset(c, 0, sizeof *c);
    snprintf(c->name, MAX_NAME, "%s", name);
    c->type = type;
    c->len = len;
    c->scale = scale;
    return c;
}
int column_index(TableSym *t, const char *name) {
    for (int i = 0; i < t->ncols; i++)
        if (ci_eq(t->cols[i].name, name)) return i;
    return -1;
}
ColumnSym *lookup_column(TableSym *t, const char *name) {
    int i = column_index(t, name);
    if (i >= 0) {
        char *ts = column_type_str(&t->cols[i]);
        symlog("lookup_column(%s, %s) -> %s", t->name, name, ts);
        free(ts);
        return &t->cols[i];
    }
    symlog("lookup_column(%s, %s) -> NOT FOUND", t->name, name);
    return NULL;
}
static char **dup_names(int n, char **src) {
    char **d = xmalloc(sizeof(char *) * (size_t)(n ? n : 1));
    for (int i = 0; i < n; i++) d[i] = xstrdup(src[i]);
    return d;
}
void table_add_key(TableSym *t, int kind, const char *cname, int ncols, char **cols) {
    t->keys = xrealloc(t->keys, sizeof(KeySym) * (size_t)(t->nkeys + 1));
    KeySym *k = &t->keys[t->nkeys++];
    k->kind = kind;
    k->name = xstrdup(cname);
    k->ncols = ncols;
    k->cols = dup_names(ncols, cols);
    for (int i = 0; i < ncols; i++) {
        ColumnSym *c = &t->cols[column_index(t, cols[i])];
        if (kind == C_PRIMARY) { c->primary = 1; c->not_null = 1; }
        if (kind == C_UNIQUE && ncols == 1) c->unique = 1;
    }
}
void table_add_fk(TableSym *t, const char *cname, int ncols, char **cols, const char *rtable, char **rcols) {
    t->fks = xrealloc(t->fks, sizeof(FKSym) * (size_t)(t->nfks + 1));
    FKSym *f = &t->fks[t->nfks++];
    f->name = xstrdup(cname);
    f->ncols = ncols;
    f->cols = dup_names(ncols, cols);
    f->ref_table = xstrdup(rtable);
    f->ref_cols = dup_names(ncols, rcols);
}
void table_add_check(TableSym *t, const char *cname, const char *sql, Node *expr) {
    t->checks = xrealloc(t->checks, sizeof(CheckSym) * (size_t)(t->nchecks + 1));
    CheckSym *c = &t->checks[t->nchecks++];
    c->name = xstrdup(cname);
    c->sql = xstrdup(sql);
    c->expr = expr;
}
KeySym *table_primary_key(TableSym *t) {
    for (int i = 0; i < t->nkeys; i++) if (t->keys[i].kind == C_PRIMARY) return &t->keys[i];
    return NULL;
}
char *column_type_str(ColumnSym *c) { return datatype_to_sql(c->type, c->len, c->scale); }

TableSym *table_copy(TableSym *s) {
    TableSym *t = table_new(s->name);
    for (int i = 0; i < s->ncols; i++) {
        ColumnSym *c = insert_column(t, s->cols[i].name, s->cols[i].type, s->cols[i].len, s->cols[i].scale);
        *c = s->cols[i];
        c->default_sql = xstrdup(s->cols[i].default_sql);
    }
    for (int i = 0; i < s->nkeys; i++) {
        t->keys = xrealloc(t->keys, sizeof(KeySym) * (size_t)(t->nkeys + 1));
        KeySym *k = &t->keys[t->nkeys++];
        k->kind = s->keys[i].kind;
        k->name = xstrdup(s->keys[i].name);
        k->ncols = s->keys[i].ncols;
        k->cols = dup_names(k->ncols, s->keys[i].cols);
    }
    for (int i = 0; i < s->nfks; i++)
        table_add_fk(t, s->fks[i].name, s->fks[i].ncols, s->fks[i].cols, s->fks[i].ref_table, s->fks[i].ref_cols);
    for (int i = 0; i < s->nchecks; i++)
        table_add_check(t, s->checks[i].name, s->checks[i].sql, node_clone(s->checks[i].expr));
    return t;
}

/* canonical CREATE TABLE text for the catalog */
char *table_to_sql(TableSym *t) {
    SB b; sb_init(&b);
    sb_add(&b, "CREATE TABLE %s (", t->name);
    for (int i = 0; i < t->ncols; i++) {
        ColumnSym *c = &t->cols[i];
        char *ts = column_type_str(c);
        sb_add(&b, "%s%s %s", i ? ", " : "", c->name, ts);
        free(ts);
        if (c->not_null && !c->primary) sb_add(&b, " NOT NULL");
        if (c->default_sql) sb_add(&b, " DEFAULT %s", c->default_sql);
    }
    for (int i = 0; i < t->nkeys; i++) {
        KeySym *k = &t->keys[i];
        sb_add(&b, ", ");
        if (k->name) sb_add(&b, "CONSTRAINT %s ", k->name);
        sb_add(&b, "%s (", k->kind == C_PRIMARY ? "PRIMARY KEY" : "UNIQUE");
        for (int j = 0; j < k->ncols; j++) sb_add(&b, "%s%s", j ? ", " : "", k->cols[j]);
        sb_add(&b, ")");
    }
    for (int i = 0; i < t->nfks; i++) {
        FKSym *f = &t->fks[i];
        sb_add(&b, ", ");
        if (f->name) sb_add(&b, "CONSTRAINT %s ", f->name);
        sb_add(&b, "FOREIGN KEY (");
        for (int j = 0; j < f->ncols; j++) sb_add(&b, "%s%s", j ? ", " : "", f->cols[j]);
        sb_add(&b, ") REFERENCES %s (", f->ref_table);
        for (int j = 0; j < f->ncols; j++) sb_add(&b, "%s%s", j ? ", " : "", f->ref_cols[j]);
        sb_add(&b, ")");
    }
    for (int i = 0; i < t->nchecks; i++) {
        sb_add(&b, ", ");
        if (t->checks[i].name) sb_add(&b, "CONSTRAINT %s ", t->checks[i].name);
        sb_add(&b, "CHECK (%s)", t->checks[i].sql);
    }
    sb_add(&b, ")");
    return sb_take(&b);
}

/* ======================================================================
 *  Views / indexes
 * ====================================================================== */
void insert_view(ViewSym *v) { ht_put(g_cat.views, v->name, v); symlog("insert_view(%s)", v->name); }
ViewSym *lookup_view(const char *name) {
    ViewSym *v = ht_get(g_cat.views, name);
    if (v) symlog("lookup_view(%s) -> FOUND", name);
    return v;
}
void delete_view(const char *name) { ht_del(g_cat.views, name); symlog("delete_view(%s)", name); }

void insert_index(IndexSym *ix) { ht_put(g_cat.indexes, ix->name, ix); symlog("insert_index(%s ON %s)", ix->name, ix->table); }
IndexSym *lookup_index(const char *name) {
    IndexSym *ix = ht_get(g_cat.indexes, name);
    symlog("lookup_index(%s) -> %s", name, ix ? "FOUND" : "NOT FOUND");
    return ix;
}
void delete_index(const char *name) { ht_del(g_cat.indexes, name); symlog("delete_index(%s)", name); }

/* ======================================================================
 *  Users and privileges:   user -> [ (object, privilege mask) ... ]
 * ====================================================================== */
UserSym *insert_user(const char *name) {
    UserSym *u = xcalloc(1, sizeof(UserSym));
    snprintf(u->name, MAX_NAME, "%s", name);
    ht_put(g_cat.users, name, u);
    symlog("insert_user(%s)", name);
    return u;
}
UserSym *lookup_user(const char *name) {
    UserSym *u = ht_get(g_cat.users, name);
    symlog("lookup_user(%s) -> %s", name, u ? "FOUND" : "NOT FOUND");
    return u;
}
void delete_user(const char *name) { ht_del(g_cat.users, name); symlog("delete_user(%s)", name); }

static PrivEntry *find_priv(UserSym *u, const char *object) {
    for (int i = 0; i < u->npriv; i++) if (ci_eq(u->privs[i].object, object)) return &u->privs[i];
    return NULL;
}
void grant_privilege(const char *user, const char *object, int privs) {
    UserSym *u = ht_get(g_cat.users, user);
    if (!u) u = insert_user(user);
    PrivEntry *p = find_priv(u, object);
    if (!p) {
        u->privs = xrealloc(u->privs, sizeof(PrivEntry) * (size_t)(u->npriv + 1));
        p = &u->privs[u->npriv++];
        snprintf(p->object, MAX_NAME, "%s", object);
        p->privs = 0;
    }
    p->privs |= privs;
    char *s = priv_mask_str(privs);
    symlog("grant_privilege(%s, %s, %s)", user, s, object);
    free(s);
}
int revoke_privilege(const char *user, const char *object, int privs) {
    UserSym *u = ht_get(g_cat.users, user);
    char *s = priv_mask_str(privs);
    symlog("revoke_privilege(%s, %s, %s)", user, s, object);
    free(s);
    if (!u) return 0;
    PrivEntry *p = find_priv(u, object);
    if (!p) return 0;
    int removed = p->privs & privs;
    p->privs &= ~privs;
    if (p->privs == 0) {                     /* compact the list */
        int idx = (int)(p - u->privs);
        memmove(&u->privs[idx], &u->privs[idx + 1], sizeof(PrivEntry) * (size_t)(u->npriv - idx - 1));
        u->npriv--;
    }
    return removed;
}
int user_privs_on(const char *user, const char *object) {
    UserSym *u = ht_get(g_cat.users, user);
    if (!u) return 0;
    if (u->is_admin) return P_ALL;
    PrivEntry *p = find_priv(u, object);
    return p ? p->privs : 0;
}
int has_privilege(const char *user, const char *object, int priv) {
    return (user_privs_on(user, object) & priv) == priv;
}
void rename_object_privileges(const char *old, const char *nw) {
    int n;
    char **keys = ht_sorted_keys(g_cat.users, &n);
    for (int i = 0; i < n; i++) {
        UserSym *u = ht_get(g_cat.users, keys[i]);
        PrivEntry *p = find_priv(u, old);
        if (p) snprintf(p->object, MAX_NAME, "%s", nw);
    }
    free(keys);
}
void drop_object_privileges(const char *object) {
    int n;
    char **keys = ht_sorted_keys(g_cat.users, &n);
    for (int i = 0; i < n; i++) revoke_privilege(keys[i], object, P_ALL);
    free(keys);
}

/* ======================================================================
 *  Persistence
 *
 *  __sqlc_catalog(seq, kind, name, def)
 *     TABLE  name  CREATE TABLE ... (canonical, re-parsed on load)
 *     VIEW   name  SELECT ...       (view definition)
 *     INDEX  name  CREATE [UNIQUE] INDEX ...
 *     USER   name  -
 *     PRIV   user  object|mask
 * ====================================================================== */
static int exec_sql(sqlite3 *db, const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "catalog: %s\n", err ? err : "error");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}
static void put_row(sqlite3_stmt *st, int seq, const char *kind, const char *name, const char *def) {
    sqlite3_bind_int(st, 1, seq);
    sqlite3_bind_text(st, 2, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, def ? def : "", -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_reset(st);
}

/* tables must be written so that referenced tables come first */
static void save_table_rec(TableSym *t, sqlite3_stmt *st, int *seq, PtrVec *done) {
    if (pv_contains_str(done, t->name)) return;
    pv_push(done, t->name);
    for (int i = 0; i < t->nfks; i++) {
        TableSym *r = ht_get(g_cat.tables, t->fks[i].ref_table);
        if (r && r != t) save_table_rec(r, st, seq, done);
    }
    char *sql = table_to_sql(t);
    put_row(st, (*seq)++, "TABLE", t->name, sql);
    free(sql);
}

int symtab_save(sqlite3 *db) {
    if (exec_sql(db, "CREATE TABLE IF NOT EXISTS __sqlc_catalog(seq INTEGER, kind TEXT, name TEXT, def TEXT)")) return -1;
    if (exec_sql(db, "DELETE FROM __sqlc_catalog")) return -1;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "INSERT INTO __sqlc_catalog VALUES (?,?,?,?)", -1, &st, NULL) != SQLITE_OK) return -1;
    int seq = 1, n;
    PtrVec done = {0};
    char **keys = ht_sorted_keys(g_cat.tables, &n);
    for (int i = 0; i < n; i++) save_table_rec(ht_get(g_cat.tables, keys[i]), st, &seq, &done);
    free(keys);
    free(done.items);
    keys = ht_sorted_keys(g_cat.views, &n);
    for (int i = 0; i < n; i++) {
        ViewSym *v = ht_get(g_cat.views, keys[i]);
        put_row(st, seq++, "VIEW", v->name, v->sql);
    }
    free(keys);
    keys = ht_sorted_keys(g_cat.indexes, &n);
    for (int i = 0; i < n; i++) {
        IndexSym *ix = ht_get(g_cat.indexes, keys[i]);
        SB b; sb_init(&b);
        sb_add(&b, "CREATE %sINDEX %s ON %s (", ix->unique ? "UNIQUE " : "", ix->name, ix->table);
        for (int j = 0; j < ix->ncols; j++) sb_add(&b, "%s%s", j ? ", " : "", ix->cols[j]);
        sb_add(&b, ")");
        char *s = sb_take(&b);
        put_row(st, seq++, "INDEX", ix->name, s);
        free(s);
    }
    free(keys);
    keys = ht_sorted_keys(g_cat.users, &n);
    for (int i = 0; i < n; i++) {
        UserSym *u = ht_get(g_cat.users, keys[i]);
        if (u->is_admin) continue;
        put_row(st, seq++, "USER", u->name, "");
        for (int j = 0; j < u->npriv; j++) {
            char *d = xsprintf("%s|%d", u->privs[j].object, u->privs[j].privs);
            put_row(st, seq++, "PRIV", u->name, d);
            free(d);
        }
    }
    free(keys);
    sqlite3_finalize(st);
    return 0;
}

int symtab_load(sqlite3 *db) {
    symtab_clear();
    int saved_log = g_symlog;
    g_symlog = 0;
    exec_sql(db, "CREATE TABLE IF NOT EXISTS __sqlc_catalog(seq INTEGER, kind TEXT, name TEXT, def TEXT)");
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "SELECT kind, name, def FROM __sqlc_catalog ORDER BY seq", -1, &st, NULL) != SQLITE_OK) {
        g_symlog = saved_log;
        return -1;
    }
    int bad = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *kind = (const char *)sqlite3_column_text(st, 0);
        const char *name = (const char *)sqlite3_column_text(st, 1);
        const char *def = (const char *)sqlite3_column_text(st, 2);
        if (strcmp(kind, "TABLE") == 0) {
            Node *ast = parse_string(def, 0);
            TableSym *t = ast ? sem_build_table(ast, 0) : NULL;
            if (t) insert_table(t); else bad++;
        } else if (strcmp(kind, "VIEW") == 0) {
            ViewSym *v = xcalloc(1, sizeof(ViewSym));
            snprintf(v->name, MAX_NAME, "%s", name);
            v->sql = xstrdup(def);
            insert_view(v);
        } else if (strcmp(kind, "INDEX") == 0) {
            Node *ast = parse_string(def, 0);
            if (!ast || ast->type != N_CREATE_INDEX) { bad++; continue; }
            IndexSym *ix = xcalloc(1, sizeof(IndexSym));
            snprintf(ix->name, MAX_NAME, "%s", ast->s1);
            snprintf(ix->table, MAX_NAME, "%s", ast->s2);
            ix->unique = ast->flag;
            ix->ncols = ast->kids[0]->nkids;
            ix->cols = xmalloc(sizeof(char *) * (size_t)ix->ncols);
            for (int i = 0; i < ix->ncols; i++) ix->cols[i] = xstrdup(ast->kids[0]->kids[i]->s1);
            insert_index(ix);
        } else if (strcmp(kind, "USER") == 0) {
            if (!ht_get(g_cat.users, name)) insert_user(name);
        } else if (strcmp(kind, "PRIV") == 0) {
            const char *bar = strrchr(def, '|');
            if (!bar) continue;
            char *obj = xstrndup(def, (size_t)(bar - def));
            grant_privilege(name, obj, atoi(bar + 1));
            free(obj);
        }
    }
    sqlite3_finalize(st);
    g_symlog = saved_log;
    if (bad) fprintf(stderr, "warning: %d catalog entr%s could not be loaded\n", bad, bad == 1 ? "y" : "ies");
    return 0;
}

/* ======================================================================
 *  Display
 * ====================================================================== */
void symtab_print_table(TableSym *t) {
    printf("  %sTABLE %s%s\n", COL_BOLD, t->name, COL_RESET);
    printf("  %-3s %-18s %-14s %-4s %-8s %-6s %s\n", "#", "COLUMN", "TYPE", "PK", "NOT NULL", "UNIQUE", "DEFAULT");
    for (int i = 0; i < t->ncols; i++) {
        ColumnSym *c = &t->cols[i];
        char *ts = column_type_str(c);
        printf("  %-3d %-18s %-14s %-4s %-8s %-6s %s\n", i + 1, c->name, ts,
               c->primary ? "YES" : "", c->not_null ? "YES" : "", c->unique ? "YES" : "",
               c->default_sql ? c->default_sql : "");
        free(ts);
    }
    for (int i = 0; i < t->nkeys; i++) {
        if (t->keys[i].ncols == 1 && t->keys[i].kind == C_UNIQUE) continue;
        printf("  %s KEY (", t->keys[i].kind == C_PRIMARY ? "PRIMARY" : "UNIQUE");
        for (int j = 0; j < t->keys[i].ncols; j++) printf("%s%s", j ? ", " : "", t->keys[i].cols[j]);
        printf(")\n");
    }
    for (int i = 0; i < t->nfks; i++) {
        printf("  FOREIGN KEY (");
        for (int j = 0; j < t->fks[i].ncols; j++) printf("%s%s", j ? ", " : "", t->fks[i].cols[j]);
        printf(") -> %s(", t->fks[i].ref_table);
        for (int j = 0; j < t->fks[i].ncols; j++) printf("%s%s", j ? ", " : "", t->fks[i].ref_cols[j]);
        printf(")\n");
    }
    for (int i = 0; i < t->nchecks; i++) printf("  CHECK (%s)\n", t->checks[i].sql);
}

void symtab_dump(void) {
    int n;
    char **keys;
    printf("%sDATABASE:%s %s    %sCURRENT USER:%s %s\n\n", COL_BOLD, COL_RESET, g_cat.dbname,
           COL_BOLD, COL_RESET, g_current_user);
    keys = ht_sorted_keys(g_cat.tables, &n);
    printf("%s-- Tables (%d) --%s\n", COL_CYAN, n, COL_RESET);
    for (int i = 0; i < n; i++) { symtab_print_table(ht_get(g_cat.tables, keys[i])); printf("\n"); }
    free(keys);
    keys = ht_sorted_keys(g_cat.views, &n);
    printf("%s-- Views (%d) --%s\n", COL_CYAN, n, COL_RESET);
    for (int i = 0; i < n; i++) {
        ViewSym *v = ht_get(g_cat.views, keys[i]);
        printf("  %s AS %s\n", v->name, v->sql);
    }
    free(keys);
    keys = ht_sorted_keys(g_cat.indexes, &n);
    printf("%s-- Indexes (%d) --%s\n", COL_CYAN, n, COL_RESET);
    for (int i = 0; i < n; i++) {
        IndexSym *ix = ht_get(g_cat.indexes, keys[i]);
        printf("  %s%s ON %s(", ix->unique ? "UNIQUE " : "", ix->name, ix->table);
        for (int j = 0; j < ix->ncols; j++) printf("%s%s", j ? ", " : "", ix->cols[j]);
        printf(")\n");
    }
    free(keys);
    keys = ht_sorted_keys(g_cat.users, &n);
    printf("%s-- Users & privileges (%d) --%s\n", COL_CYAN, n, COL_RESET);
    for (int i = 0; i < n; i++) {
        UserSym *u = ht_get(g_cat.users, keys[i]);
        printf("  %s%s\n", u->name, u->is_admin ? "  (administrator: all privileges)" : "");
        for (int j = 0; j < u->npriv; j++) {
            char *s = priv_mask_str(u->privs[j].privs);
            printf("   %s%s -> %s\n", tree_branch(j == u->npriv - 1), s, u->privs[j].object);
            free(s);
        }
    }
    free(keys);
}
