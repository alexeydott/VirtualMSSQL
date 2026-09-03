/* vms_client.h — portable client runtime API (R3).
 *
 * Layering per TZ: nothing in this header mentions ODBC. The adapter and
 * worker live in vms_odbc_adapter.c.
 *
 * Invariants enforced here:
 *   - one owner per connection / statement handle
 *   - one cursor -> one statement
 *   - a row becomes visible only after complete decode of all columns
 *   - decoded values are owned by the statement and invalidated by the
 *     next fetch / more_results / destroy
 */
#ifndef VIRTUALMSSQL_VMS_CLIENT_H
#define VIRTUALMSSQL_VMS_CLIENT_H

#include "vms_error.h"

/* metadata column flavor lives in vms_meta.h; the cursor API takes it */
typedef struct VmsMetaColumn VmsMetaColumn;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VmsClient VmsClient;
typedef struct VmsConnection VmsConnection;
typedef struct VmsStatement VmsStatement;

/* ---- values ---- */
typedef enum VmsValType {
    VMS_VAL_NULL = 0,
    VMS_VAL_INT64,
    VMS_VAL_FLOAT64,
    VMS_VAL_TEXT,     /* UTF-8 */
    VMS_VAL_BLOB,
    VMS_VAL_BIGTEXT   /* text that arrived via SQLGetData streaming */
} VmsValType;

typedef struct VmsValue {
    VmsValType type;
    long long i;
    double f;
    char* text;        /* UTF-8, NUL-terminated, length in text_len */
    size_t text_len;
    unsigned char* blob;
    size_t blob_len;
} VmsValue;

/* ---- column metadata ---- */
typedef enum VmsColType {
    VMS_CT_INT64 = 0,
    VMS_CT_FLOAT64,
    VMS_CT_TEXT,       /* varchar/nchar/... and small LOBs */
    VMS_CT_BIGTEXT,    /* n/varchar(max), xml, ntext */
    VMS_CT_BLOB,       /* binary/varbinary(max) */
    VMS_CT_DECIMAL,    /* decoded as exact TEXT */
    VMS_CT_DATETIME,   /* decoded as ISO TEXT */
    VMS_CT_GUID        /* decoded as canonical TEXT */
} VmsColType;

typedef struct VmsColumnMeta {
    char name[128];    /* UTF-8 */
    VmsColType type;
    int nullable;
    unsigned long col_size;  /* provider-reported size/precision */
    unsigned short decimal_digits;
} VmsColumnMeta;

/* ---- client (environment owner) ---- */
VmsClient* vms_client_init(VmsError* err);
void vms_client_destroy(VmsClient* c);

/* ---- connection ----
 * connstr_w is the strict connection string (UTF-16). The adapter appends
 * nothing: the caller owns the exact bytes. */
VmsConnection* vms_conn_open(VmsClient* c, const wchar_t* connstr_w, VmsError* err);
void vms_conn_close(VmsConnection* cn);
int vms_conn_quarantined(const VmsConnection* cn);
/* reset quarantine decision point (R14 policy; R3 only reports) */

/* ---- statements ----
 * Both calls run prepare+execute on the connection's worker and return a
 * statement positioned before the first result row. */
VmsStatement* vms_stmt_exec_direct(VmsConnection* cn, const wchar_t* sql_w,
                                   VmsError* err);
VmsStatement* vms_stmt_exec_params(VmsConnection* cn, const wchar_t* sql_w,
                                   const VmsValue* params, int nparams,
                                   VmsError* err);
void vms_stmt_destroy(VmsStatement* st);

/* fetch: 1 = row ready (fully decoded), 0 = end of result set, -1 = error */
int vms_stmt_fetch(VmsStatement* st, VmsError* err);

/* move to the next result set: 1 = more, 0 = none, -1 = error */
int vms_stmt_more_results(VmsStatement* st, VmsError* err);

int vms_stmt_col_count(const VmsStatement* st);
const VmsColumnMeta* vms_stmt_meta(const VmsStatement* st, int col);
const VmsValue* vms_stmt_value(const VmsStatement* st, int col);

/* thread-safe: may be called from any host thread while another thread is
 * blocked in vms_stmt_fetch/exec. 1 = attention delivered. */
int vms_stmt_cancel(VmsStatement* st);
/* sqlite3_interrupt-style: cancel whatever the connection's worker is doing
 * right now (exec/fetch/more). Safe from any thread, no statement needed. */
int vms_conn_cancel(VmsConnection* cn);

/* ---- transactions ----
 * vms_tran_begin switches the connection to manual-commit mode; commit and
 * rollback leave the mode set. The default mode is autocommit. */
int vms_tran_begin(VmsConnection* cn, VmsError* err);
int vms_tran_commit(VmsConnection* cn, VmsError* err);
int vms_tran_rollback(VmsConnection* cn, VmsError* err);

/* ---- pool support ----
 * Clean-state verification for reuse decisions: not quarantined,
 * @@TRANCOUNT == 0, SELECT 1 round-trip works. 1 = clean.
 * Connection reuse is allowed only with a proven clean state. */
int vms_conn_verify(VmsConnection* cn);

/* ---- DML execution (R10 write path; adapter-side) ----
 * Execute a parameterized DML statement. ntotal SQL slots are filled in
 * statement order: torder[i] >= 0 -> int param torder[i]; torder[i] < 0 ->
 * text param -(torder[i])-1. rows_affected receives SQLRowCount (-1 on
 * failure). Values reach the server only as bound parameters. */
int vms_conn_exec_dml(VmsConnection* cn, const wchar_t* sql,
                      const long long* iparams, int nint,
                      const wchar_t* const* tparams, const int* tlengths, int ntext,
                      const int* torder, int ntotal,
                      long long* rows_affected, VmsError* err);

/* ---- R11 explicit transactions (pinned connection) ----
 * A transaction pins one canonical SQL Server identity for its whole
 * lifetime: autocommit is switched off and every DML joins the remote
 * transaction lazily (the first statement issues BEGIN). Finalization is
 * non-cancellable: once a COMMIT/ROLLBACK wire operation starts it runs to
 * completion; an interrupted COMMIT yields an unknown outcome and the
 * connection is quarantined permanently (never returned to the pool). */

/* result classification for txn finalization */
typedef enum VmsTxnResult {
    VMS_TXN_OK = 0,        /* committed / rolled back cleanly */
    VMS_TXN_BUSY,          /* conflicting active op / lock timeout (retryable) */
    VMS_TXN_ROLLED_BACK,   /* commit refused: doomed txn rolled back instead */
    VMS_TXN_UNKNOWN        /* outcome unknown: connection is quarantined */
} VmsTxnResult;

/* switch the connection into manual-commit mode and apply the transaction
 * primer (XACT_ABORT ON, AUTOBEGINTXN OFF). The connection must not be
 * returned to the pool while the transaction is open. */
int vms_txn_pin(VmsConnection* cn, VmsError* err);
/* lazily start the remote transaction (BEGIN TRAN) if not yet started;
 * idempotent: safe to call before every DML. */
int vms_txn_begin_lazy(VmsConnection* cn, VmsError* err);
/* 1 when the remote transaction is open on this connection. */
int vms_txn_active(VmsConnection* cn);
/* execute a savepoint statement: name must be a validated identifier.
 * op: 0 = SAVE TRANSACTION name, 1 = ROLLBACK TRANSACTION name. */
int vms_txn_savepoint(VmsConnection* cn, const char* name, int rollback_op,
                      VmsError* err);
/* 1 when the server reports an uncommittable transaction
 * (XACT_STATE() == -1). 0 otherwise (including on errors). */
int vms_txn_doomed(const VmsConnection* cn);
/* validation-only check run from xSync: active + not doomed. */
int vms_txn_validate(const VmsConnection* cn);
/* finalize: COMMIT or ROLLBACK, then restore autocommit. Non-cancellable.
 * returns VMS_TXN_OK / VMS_TXN_BUSY / VMS_TXN_UNKNOWN. */
VmsTxnResult vms_txn_commit(VmsConnection* cn, VmsError* err);
VmsTxnResult vms_txn_rollback(VmsConnection* cn, VmsError* err);

/* ---- vtab read cursor (R6) ----
 * A cursor is an independent lease over its own ODBC connection lease:
 * two cursors scan concurrently (SQLite nested scans) without sharing
 * statement handles. Rows stream through the cursor's own worker. */
typedef struct VmsCursor VmsCursor;

/* open a cursor streaming "SELECT <quoted column list> FROM <quoted table>"
 * built from validated identifiers; each cursor holds its own pool lease
 * for the duration of the scan. params/nparams carry the R7 pushdown
 * values (int64 equality/range/IN/limit/offset in plan order). */
VmsCursor* vms_cursor_open(VmsConnection* cn, const char* schema,
                           const char* table, const VmsMetaColumn* cols,
                           int ncols, VmsError* err);
/* open with an explicit remote SQL statement and bound integer params. */
VmsCursor* vms_cursor_open_sql(VmsConnection* cn, const wchar_t* sql,
                               const long long* params, int nparams,
                               VmsError* err);
/* R11: open a cursor that shares the parent connection's HDBC (MARS). Its
 * reads participate in the transaction pinned on that connection and see
 * the transaction's own uncommitted writes. The parent connection must
 * outlive the cursor; cursors on one HDBC are serialized by its worker. */
VmsCursor* vms_cursor_open_shared(VmsConnection* cn, const wchar_t* sql,
                                  const long long* params, int nparams,
                                  VmsError* err);
void vms_cursor_close(VmsCursor* cur);
/* fetch next row: 1 = row ready (fully decoded), 0 = end, -1 = error */
int vms_cursor_fetch(VmsCursor* cur, VmsError* err);
/* cancel the scan from any thread (sqlite3_interrupt path) */
int vms_cursor_cancel(VmsCursor* cur);
int vms_cursor_col_count(const VmsCursor* cur);
const VmsColumnMeta* vms_cursor_meta(const VmsCursor* cur, int col);
const VmsValue* vms_cursor_value(const VmsCursor* cur, int col);

#ifdef __cplusplus
}
#endif

#endif /* VIRTUALMSSQL_VMS_CLIENT_H */
