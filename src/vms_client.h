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

#ifdef __cplusplus
}
#endif

#endif /* VIRTUALMSSQL_VMS_CLIENT_H */
