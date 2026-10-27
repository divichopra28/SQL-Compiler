/*
 * transaction.c - transaction manager implementation
 */
#include "transaction.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

Transaction g_txn;
static sqlite3 *tdb = NULL;
static int next_id = 1;

static int run(const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(tdb, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "storage: %s (%s)\n", err ? err : "error", sql);
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static void clear_ops(void) {
    for (int i = 0; i < g_txn.nops; i++) free(g_txn.ops[i]);
    free(g_txn.ops);
    g_txn.ops = NULL;
    g_txn.nops = 0;
}

void txn_init(sqlite3 *db) {
    tdb = db;
    if (g_txn.state == TXN_ACTIVE) clear_ops();
    g_txn.state = TXN_NONE;
    g_txn.nsp = 0;
    g_txn.implicit = 0;
}

const char *txn_state_name(TxnState s) {
    switch (s) {
    case TXN_ACTIVE: return "ACTIVE";
    case TXN_COMMITTED: return "COMMITTED";
    case TXN_ROLLED_BACK: return "ROLLED_BACK";
    default: return "NONE (autocommit)";
    }
}

int txn_active(void) { return g_txn.state == TXN_ACTIVE; }

int txn_begin(int implicit) {
    if (txn_active()) return -1;
    if (run("BEGIN")) return -1;
    clear_ops();
    g_txn.id = next_id++;
    g_txn.state = TXN_ACTIVE;
    g_txn.implicit = implicit;
    g_txn.nsp = 0;
    return 0;
}

int txn_commit(void) {
    if (!txn_active()) return -1;
    if (run("COMMIT")) return -1;
    g_txn.state = TXN_COMMITTED;
    g_txn.nsp = 0;
    return 0;
}

int txn_rollback(void) {
    if (!txn_active()) return -1;
    if (run("ROLLBACK")) return -1;
    g_txn.state = TXN_ROLLED_BACK;
    g_txn.nsp = 0;
    return 0;
}

static int find_sp(const char *name) {
    for (int i = g_txn.nsp - 1; i >= 0; i--)
        if (ci_eq(g_txn.sp[i].name, name)) return i;
    return -1;
}
int txn_has_savepoint(const char *name) { return find_sp(name) >= 0; }

int txn_savepoint(const char *name) {
    if (!txn_active() || g_txn.nsp >= 64) return -1;
    char *sql = xsprintf("SAVEPOINT \"%s\"", name);
    int rc = run(sql);
    free(sql);
    if (rc) return -1;
    Savepoint *s = &g_txn.sp[g_txn.nsp++];
    snprintf(s->name, sizeof s->name, "%s", name);
    s->op_mark = g_txn.nops;
    return 0;
}

int txn_rollback_to(const char *name) {
    int i = find_sp(name);
    if (i < 0) return -1;
    char *sql = xsprintf("ROLLBACK TO \"%s\"", name);
    int rc = run(sql);
    free(sql);
    if (rc) return -1;
    /* discard operations logged after the savepoint and later savepoints */
    for (int k = g_txn.sp[i].op_mark; k < g_txn.nops; k++) free(g_txn.ops[k]);
    g_txn.nops = g_txn.sp[i].op_mark;
    g_txn.nsp = i + 1;
    return 0;
}

int txn_release(const char *name) {
    int i = find_sp(name);
    if (i < 0) return -1;
    char *sql = xsprintf("RELEASE \"%s\"", name);
    int rc = run(sql);
    free(sql);
    if (rc) return -1;
    g_txn.nsp = i;
    return 0;
}

void txn_log(const char *fmt, ...) {
    if (!txn_active()) return;
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_txn.ops = xrealloc(g_txn.ops, sizeof(char *) * (size_t)(g_txn.nops + 1));
    g_txn.ops[g_txn.nops++] = xstrdup(buf);
}

void txn_print_status(void) {
    if (g_txn.id == 0) {
        printf("Transaction: none started in this session (autocommit mode)\n");
        return;
    }
    printf("Transaction ID    : %d\n", g_txn.id);
    printf("Transaction State : %s%s\n", txn_state_name(g_txn.state),
           g_txn.implicit && txn_active() ? " (started implicitly by SAVEPOINT)" : "");
    printf("Savepoints        : ");
    if (!g_txn.nsp) printf("(none)");
    for (int i = 0; i < g_txn.nsp; i++) printf("%s%s", i ? " -> " : "", g_txn.sp[i].name);
    printf("\n");
    printf("Pending Operations: %d\n", txn_active() ? g_txn.nops : 0);
    if (txn_active()) {
        int spi = 0;
        for (int i = 0; i < g_txn.nops; i++) {
            while (spi < g_txn.nsp && g_txn.sp[spi].op_mark == i) printf("    -- SAVEPOINT %s --\n", g_txn.sp[spi++].name);
            printf("    %d. %s\n", i + 1, g_txn.ops[i]);
        }
        while (spi < g_txn.nsp) printf("    -- SAVEPOINT %s --\n", g_txn.sp[spi++].name);
    }
}

/* ---- statement atomicity ---- */
int stmt_begin(void) {
    if (txn_active()) return run("SAVEPOINT __sqlc_stmt");
    return run("BEGIN");
}
int stmt_commit(void) {
    if (txn_active()) return run("RELEASE __sqlc_stmt");
    return run("COMMIT");
}
int stmt_rollback(void) {
    if (txn_active()) {
        run("ROLLBACK TO __sqlc_stmt");
        return run("RELEASE __sqlc_stmt");
    }
    return run("ROLLBACK");
}
