/*
 * transaction.h - transaction manager
 *
 * Tracks the current transaction: id, state, savepoints and the list of
 * operations performed inside it. Durability and undo are delegated to
 * the storage engine (SQLite BEGIN / SAVEPOINT / ROLLBACK TO), while this
 * module enforces the rules (no nested BEGIN, unknown savepoints, ...)
 * and gives every statement atomicity (a failing statement is undone
 * without aborting the surrounding transaction).
 */
#ifndef TRANSACTION_H
#define TRANSACTION_H

#include <sqlite3.h>

typedef enum { TXN_NONE = 0, TXN_ACTIVE, TXN_COMMITTED, TXN_ROLLED_BACK } TxnState;

typedef struct {
    char name[64];
    int  op_mark;        /* number of logged operations when created */
} Savepoint;

typedef struct {
    int        id;
    TxnState   state;
    int        implicit;   /* started automatically by SAVEPOINT */
    Savepoint  sp[64];
    int        nsp;
    char     **ops;
    int        nops;
} Transaction;

extern Transaction g_txn;

void txn_init(sqlite3 *db);
int  txn_active(void);
int  txn_begin(int implicit);
int  txn_commit(void);
int  txn_rollback(void);
int  txn_savepoint(const char *name);
int  txn_rollback_to(const char *name);
int  txn_release(const char *name);
int  txn_has_savepoint(const char *name);
void txn_log(const char *fmt, ...);
void txn_print_status(void);
const char *txn_state_name(TxnState s);

/* statement-level atomicity */
int  stmt_begin(void);
int  stmt_commit(void);
int  stmt_rollback(void);

#endif
