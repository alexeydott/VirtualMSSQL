/* vms_odbc_adapter.c — ODBC client runtime (R3).
 *
 * ODBC types are confined to this file per the TZ layering rule.
 * Handles ownership: VmsConnection owns SQLHDBC, VmsStatement owns SQLHSTMT,
 * all calls run on the connection's worker; cancel goes through the worker's
 * published active statement (R0-proven scheme).
 * Rows become visible only after complete decode of every column. */
#include "vms_client.h"
#include "vms_foundation.h"
#include "vms_odbc_worker.h"
#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#include <sqlucode.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

/* SQL Server driver-specific ODBC C-type constants absent from sqlext.h */
#ifndef SQL_SS_VARIANT
#define SQL_SS_VARIANT (-150)
#endif
#ifndef SQL_SS_TIME2
#define SQL_SS_TIME2 (-154)
#endif
#ifndef SQL_SS_TIMESTAMPOFFSET
#define SQL_SS_TIMESTAMPOFFSET (-155)
#endif
#ifndef SQL_SS_XML
#define SQL_SS_XML (-152)
#endif
#ifndef SQL_SS_LENGTH_UNLIMITED
#define SQL_SS_LENGTH_UNLIMITED 0
#endif
/* R0 finding: the driver rejects ColumnSize=0 for LONG parameter types;
 * 2^30-1 is the de-facto "unlimited" precision it accepts. */
#define VMS_PARAM_UNLIMITED ((SQLULEN)1073741823)
#define VMS_GETDATA_CHUNK 65536

/* ================= error helpers ================= */

void vms_error_ok(VmsError* e)
{
    if (e) { memset(e, 0, sizeof(*e)); e->cls = VMS_OK; }
}

void vms_error_set(VmsError* e, VmsErrClass cls, const char* sqlstate,
                   int native, const char* fmt, ...)
{
    va_list ap;
    if (!e) return;
    memset(e, 0, sizeof(*e));
    e->cls = cls;
    if (sqlstate) { strncpy_s(e->sqlstate, sizeof(e->sqlstate), sqlstate, _TRUNCATE); }
    else { strcpy_s(e->sqlstate, sizeof(e->sqlstate), "00000"); }
    e->native = native;
    va_start(ap, fmt);
    _vsnprintf_s(e->message, sizeof(e->message), _TRUNCATE, fmt, ap);
    va_end(ap);
}

static VmsErrClass classify_state(const char* st, int* quarantine)
{
    *quarantine = 0;
    if (!st || !st[0]) return VMS_ERR_INTERNAL;
    if (!strncmp(st, "IM0", 3)) {
        /* IM001/IM002/IM003: driver/DNS problems -> treated as driver missing */
        if (!strcmp(st, "IM002") || !strcmp(st, "IM003")) return VMS_ERR_DRIVER_NOT_FOUND;
        return VMS_ERR_CONNECT;
    }
    if (!strcmp(st, "HY008")) return VMS_ERR_CANCELLED;
    if (!strcmp(st, "HYT00") || !strcmp(st, "HYT01")) return VMS_ERR_TIMEOUT;
    if (!strncmp(st, "08", 2)) { *quarantine = 1; return VMS_ERR_TRANSPORT; }
    return VMS_ERR_EXEC;
}

/* capture ODBC diagnostics; returns class and flags quarantine */
static VmsErrClass diag_capture(VmsError* err, SQLSMALLINT htype, SQLHANDLE h,
                                const char* what, int* quarantine)
{
    SQLWCHAR state[6];
    SQLWCHAR msg[1024];
    SQLINTEGER native = 0;
    SQLSMALLINT len = 0;
    SQLRETURN r;
    char st_u8[8] = { 0 };
    char msg_u8[512] = { 0 };

    *quarantine = 0;
    r = SQLGetDiagRecW(htype, h, 1, state, &native, msg,
                       (SQLSMALLINT)(sizeof(msg) / sizeof(msg[0])), &len);
    if (SQL_SUCCEEDED(r)) {
        WideCharToMultiByte(CP_UTF8, 0, (LPCWCH)state, -1, st_u8, sizeof(st_u8), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, (LPCWCH)msg, -1, msg_u8, sizeof(msg_u8), NULL, NULL);
    }
    {
        VmsErrClass cls = classify_state(st_u8, quarantine);
        vms_error_set(err, cls, st_u8, (int)native, "%s: %s", what,
                      msg_u8[0] ? msg_u8 : "no ODBC diagnostics available");
        return cls;
    }
}

/* ================= objects ================= */

struct VmsClient {
    HENV env; /* SQLHENV */
};

struct VmsConnection {
    VmsClient* client;
    HDBC hdbc;              /* single owner: this object */
    VmsWorker* worker;
    volatile LONG quarantined;
};

struct VmsStatement {
    VmsConnection* conn;
    HSTMT hstmt;            /* single owner: this object */
    int col_count;
    VmsColumnMeta* meta;
    VmsValue* row;
    int row_ready;
};

/* ================= value lifecycle ================= */

static void value_clear(VmsValue* v)
{
    if (v->text) { HeapFree(GetProcessHeap(), 0, v->text); v->text = NULL; }
    if (v->blob) { HeapFree(GetProcessHeap(), 0, v->blob); v->blob = NULL; }
    v->text_len = 0;
    v->blob_len = 0;
    v->type = VMS_VAL_NULL;
    v->i = 0;
    v->f = 0;
}

static void row_clear(VmsStatement* st)
{
    int i;
    if (!st->row) return;
    for (i = 0; i < st->col_count; i++) value_clear(&st->row[i]);
    st->row_ready = 0;
}

/* ================= worker jobs ================= */

typedef struct OpOpen {
    VmsConnection* cn;
    const wchar_t* connstr;
    VmsError* err;
    int ok;
} OpOpen;

static void job_open(void* arg)
{
    OpOpen* op = (OpOpen*)arg;
    SQLRETURN r;
    vms_error_ok(op->err);
    r = SQLDriverConnectW(op->cn->hdbc, NULL, (SQLWCHAR*)op->connstr, SQL_NTS,
                          NULL, 0, NULL, SQL_DRIVER_NOPROMPT);
    if (r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO) {
        op->ok = 1;
        return;
    }
    {
        int q;
        diag_capture(op->err, SQL_HANDLE_DBC, op->cn->hdbc, "connect", &q);
        if (q) InterlockedExchange(&op->cn->quarantined, 1);
    }
    op->ok = 0;
}

typedef struct OpExec {
    VmsConnection* cn;
    VmsStatement* st;      /* pre-allocated; job fills it */
    const wchar_t* sql;
    VmsError* err;
    int ok;
} OpExec;

/* NOTE (R0 lessons encoded): when parameter binding lands (R7), every
 * SQLBindParameter return must be checked (silent bind failures surface as
 * 07002 at execute) and LONG-type parameters need ColumnSize = 2^30-1
 * (VMS_PARAM_UNLIMITED); the driver rejects SQL_SS_LENGTH_UNLIMITED (0) with
 * HY104. DAE (SQLParamData/SQLPutData) is proven in R0 for oversized values. */

static void job_exec(void* arg)
{
    OpExec* op = (OpExec*)arg;
    SQLRETURN r;
    vms_error_ok(op->err);
    op->ok = 0;

    if (InterlockedCompareExchange(&op->cn->quarantined, 0, 0)) {
        vms_error_set(op->err, VMS_ERR_QUARANTINED, NULL, 0,
                      "connection quarantined; ops rejected");
        return;
    }
    vms_worker_set_active(op->cn->worker, op->st->hstmt);
    r = SQLExecDirectW(op->st->hstmt, (SQLWCHAR*)op->sql, SQL_NTS);
    vms_worker_set_active(op->cn->worker, NULL);

    if (!(r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO || r == SQL_NO_DATA)) {
        int q;
        diag_capture(op->err, SQL_HANDLE_STMT, op->st->hstmt, "execute", &q);
        if (q) InterlockedExchange(&op->cn->quarantined, 1);
        return;
    }
    op->ok = 1;
}

/* ================= column metadata ================= */

static VmsColType map_col_type(SQLSMALLINT sql_type, SQLULEN col_size)
{
    switch (sql_type) {
    case SQL_INTEGER: case SQL_SMALLINT: case SQL_TINYINT:
    case SQL_BIGINT: case SQL_BIT:
        return VMS_CT_INT64;
    case SQL_REAL: case SQL_FLOAT: case SQL_DOUBLE:
        return VMS_CT_FLOAT64;
    case SQL_DECIMAL: case SQL_NUMERIC:
        return VMS_CT_DECIMAL;
    case SQL_TYPE_DATE: case SQL_TYPE_TIME: case SQL_SS_TIME2:
    case SQL_TYPE_TIMESTAMP: case SQL_SS_TIMESTAMPOFFSET:
        return VMS_CT_DATETIME;
    case SQL_GUID:
        return VMS_CT_GUID;
    case SQL_BINARY: case SQL_VARBINARY:
    case SQL_LONGVARBINARY:
        return VMS_CT_BLOB;
    case SQL_WLONGVARCHAR: case SQL_SS_XML:
        return VMS_CT_BIGTEXT;
    case SQL_CHAR: case SQL_VARCHAR: case SQL_WCHAR: case SQL_WVARCHAR:
        return (col_size == SQL_SS_LENGTH_UNLIMITED) ? VMS_CT_BIGTEXT : VMS_CT_TEXT;
    default:
        return VMS_CT_TEXT; /* conservative: decode as text */
    }
}

static void job_meta(void* arg);

typedef struct OpMeta {
    VmsStatement* st;
    VmsError* err;
    int ok;
} OpMeta;

static void job_meta(void* arg)
{
    OpMeta* op = (OpMeta*)arg;
    SQLSMALLINT n = 0;
    int i;
    vms_error_ok(op->err);
    if (!SQL_SUCCEEDED(SQLNumResultCols(op->st->hstmt, &n))) {
        int q;
        diag_capture(op->err, SQL_HANDLE_STMT, op->st->hstmt, "SQLNumResultCols", &q);
        op->ok = 0;
        return;
    }
    op->st->col_count = n;
    if (n > 0) {
        op->st->meta = (VmsColumnMeta*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                 sizeof(VmsColumnMeta) * (size_t)n);
        if (!op->st->meta) {
            vms_error_set(op->err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM meta");
            op->ok = 0;
            return;
        }
    }
    for (i = 1; i <= n; i++) {
        SQLWCHAR name[128];
        SQLSMALLINT name_len = 0, nullable = 0, digits = 0, sql_type = 0;
        SQLULEN col_size = 0;
        char name_u8[128];
        if (!SQL_SUCCEEDED(SQLDescribeColW(op->st->hstmt, (SQLUSMALLINT)i, name,
                                           (SQLSMALLINT)(sizeof(name) / sizeof(name[0])),
                                           &name_len, &sql_type, &col_size,
                                           &digits, &nullable))) {
            int q;
            diag_capture(op->err, SQL_HANDLE_STMT, op->st->hstmt, "SQLDescribeColW", &q);
            op->ok = 0;
            return;
        }
        WideCharToMultiByte(CP_UTF8, 0, (LPCWCH)name, -1, name_u8, sizeof(name_u8), NULL, NULL);
        strncpy_s(op->st->meta[i - 1].name, sizeof(op->st->meta[i - 1].name),
                  name_u8, _TRUNCATE);
        op->st->meta[i - 1].type = map_col_type(sql_type, col_size);
        op->st->meta[i - 1].nullable = (nullable != SQL_NO_NULLS);
        op->st->meta[i - 1].col_size = (unsigned long)col_size;
        op->st->meta[i - 1].decimal_digits = (unsigned short)digits;
    }
    op->ok = 1;
}

/* ================= fetch / decode ================= */

typedef struct OpFetch {
    VmsStatement* st;
    VmsError* err;
    int result; /* 1 row, 0 done, -1 error */
} OpFetch;

/* read one column in bounded chunks (R0: ordinals must increase monotonically,
 * which this loop guarantees: 1..n) */
static int getdata_text(HSTMT h, int col, VmsValue* out, VmsError* err)
{
    wchar_t chunk[VMS_GETDATA_CHUNK / sizeof(wchar_t)];
    VmsBounded acc;
    SQLLEN got = 0;
    SQLRETURN r;
    size_t total_chars = 0;

    if (!vms_buf_init(&acc, 4096)) {
        vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM row buffer");
        return 0;
    }
    for (;;) {
        r = SQLGetData(h, (SQLUSMALLINT)col, SQL_C_WCHAR, chunk, sizeof(chunk), &got);
        if (r == SQL_NO_DATA) break;
        if (!(r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO)) {
            int q;
            diag_capture(err, SQL_HANDLE_STMT, h, "SQLGetData(text)", &q);
            vms_buf_free(&acc);
            return 0;
        }
        if (got == SQL_NULL_DATA) {
            out->type = VMS_VAL_NULL;
            vms_buf_free(&acc);
            return 1;
        }
        {
            size_t chars;
            size_t bytes8 = 0;
            if (got == SQL_NO_TOTAL) {
                /* truncated chunk: buffer holds sizeof(chunk) minus terminator */
                chars = sizeof(chunk) / sizeof(wchar_t) - 1;
            } else if (got == 0) {
                chars = 0;
            } else {
                chars = (size_t)got / sizeof(wchar_t);
            }
            if (chars > 0 && vms_utf16_to_utf8(chunk, chars, NULL, 0, &bytes8) != 0) {
                vms_error_set(err, VMS_ERR_INTERNAL, NULL, 0,
                              "utf16 size check failed (chars=%zu)", chars);
                vms_buf_free(&acc);
                return 0;
            }
            if (chars > 0) {
                char* tmp = (char*)HeapAlloc(GetProcessHeap(), 0, bytes8 + 1);
                if (!tmp) {
                    vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM chunk");
                    vms_buf_free(&acc);
                    return 0;
                }
                vms_utf16_to_utf8(chunk, chars, tmp, bytes8 + 1, &bytes8);
                if (!vms_buf_append(&acc, tmp, bytes8)) {
                    vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "row too large for buffer");
                    HeapFree(GetProcessHeap(), 0, tmp);
                    vms_buf_free(&acc);
                    return 0;
                }
                HeapFree(GetProcessHeap(), 0, tmp);
                total_chars += chars;
            }
        }
        if (r == SQL_SUCCESS) break; /* last chunk */
    }
    out->type = VMS_VAL_TEXT;
    out->text_len = vms_buf_len(&acc);
    out->text = acc.data; /* transfer ownership */
    (void)total_chars;
    return 1;
}

static int getdata_blob(HSTMT h, int col, VmsValue* out, VmsError* err)
{
    char chunk[VMS_GETDATA_CHUNK];
    VmsBounded acc;
    SQLLEN got = 0;
    SQLRETURN r;

    if (!vms_buf_init(&acc, 4096)) {
        vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM row buffer");
        return 0;
    }
    for (;;) {
        r = SQLGetData(h, (SQLUSMALLINT)col, SQL_C_BINARY, chunk, sizeof(chunk), &got);
        if (r == SQL_NO_DATA) break;
        if (!(r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO)) {
            int q;
            diag_capture(err, SQL_HANDLE_STMT, h, "SQLGetData(blob)", &q);
            vms_buf_free(&acc);
            return 0;
        }
        if (got == SQL_NULL_DATA) {
            out->type = VMS_VAL_NULL;
            vms_buf_free(&acc);
            return 1;
        }
        {
            size_t bytes = (got == SQL_NO_TOTAL) ? sizeof(chunk) : (size_t)got;
            if (!vms_buf_append(&acc, chunk, bytes)) {
                vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "row too large for buffer");
                vms_buf_free(&acc);
                return 0;
            }
        }
        if (r == SQL_SUCCESS) break;
    }
    out->type = VMS_VAL_BLOB;
    out->blob_len = vms_buf_len(&acc);
    out->blob = (unsigned char*)acc.data;
    return 1;
}

static void job_fetch(void* arg)
{
    OpFetch* op = (OpFetch*)arg;
    VmsStatement* st = op->st;
    SQLRETURN r;
    int i;

    vms_error_ok(op->err);
    row_clear(st);
    vms_worker_set_active(st->conn->worker, st->hstmt);
    r = SQLFetch(st->hstmt);
    vms_worker_set_active(st->conn->worker, NULL);
    if (r == SQL_NO_DATA) { op->result = 0; return; }
    if (!(r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO)) {
        int q;
        diag_capture(op->err, SQL_HANDLE_STMT, st->hstmt, "SQLFetch", &q);
        if (q) InterlockedExchange(&st->conn->quarantined, 1);
        op->result = -1;
        return;
    }
    /* decode COMPLETE row before making it visible (TZ invariant) */
    if (!st->row && st->col_count > 0) {
        st->row = (VmsValue*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                       sizeof(VmsValue) * (size_t)st->col_count);
        if (!st->row) {
            vms_error_set(op->err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM row");
            op->result = -1;
            return;
        }
    }
    for (i = 1; i <= st->col_count; i++) {
        VmsValue* v = &st->row[i - 1];
        const VmsColumnMeta* m = &st->meta[i - 1];
        switch (m->type) {
        case VMS_CT_INT64: {
            SQLBIGINT iv = 0;
            SQLLEN ind = 0;
            if (!SQL_SUCCEEDED(SQLGetData(st->hstmt, (SQLUSMALLINT)i, SQL_C_SBIGINT,
                                          &iv, 0, &ind))) {
                int q;
                diag_capture(op->err, SQL_HANDLE_STMT, st->hstmt, "SQLGetData(int)", &q);
                op->result = -1;
                return;
            }
            if (ind == SQL_NULL_DATA) v->type = VMS_VAL_NULL;
            else { v->type = VMS_VAL_INT64; v->i = (long long)iv; }
            break;
        }
        case VMS_CT_FLOAT64: {
            double dv = 0;
            SQLLEN ind = 0;
            if (!SQL_SUCCEEDED(SQLGetData(st->hstmt, (SQLUSMALLINT)i, SQL_C_DOUBLE,
                                          &dv, 0, &ind))) {
                int q;
                diag_capture(op->err, SQL_HANDLE_STMT, st->hstmt, "SQLGetData(float)", &q);
                op->result = -1;
                return;
            }
            if (ind == SQL_NULL_DATA) v->type = VMS_VAL_NULL;
            else { v->type = VMS_VAL_FLOAT64; v->f = dv; }
            break;
        }
        case VMS_CT_TEXT: case VMS_CT_DECIMAL: case VMS_CT_DATETIME:
        case VMS_CT_GUID: case VMS_CT_BIGTEXT:
            if (!getdata_text(st->hstmt, i, v, op->err)) { op->result = -1; return; }
            break;
        case VMS_CT_BLOB:
            if (!getdata_blob(st->hstmt, i, v, op->err)) { op->result = -1; return; }
            break;
        }
    }
    st->row_ready = 1;
    op->result = 1;
}

typedef struct OpSimple {
    VmsConnection* cn;
    VmsError* err;
    int ok;
} OpSimple;

typedef struct OpMore {
    VmsStatement* st;
    VmsError* err;
    int result; /* 1 more, 0 none, -1 error */
} OpMore;

static void job_more(void* arg)
{
    OpMore* op = (OpMore*)arg;
    SQLRETURN r;
    vms_error_ok(op->err);
    row_clear(op->st);
    r = SQLMoreResults(op->st->hstmt);
    if (r == SQL_NO_DATA) { op->result = 0; return; }
    if (!(r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO)) {
        int q;
        diag_capture(op->err, SQL_HANDLE_STMT, op->st->hstmt, "SQLMoreResults", &q);
        if (q) InterlockedExchange(&op->st->conn->quarantined, 1);
        op->result = -1;
        return;
    }
    op->result = 1;
}

static void job_begin(void* arg)
{
    OpSimple* op = (OpSimple*)arg;
    SQLRETURN r;
    vms_error_ok(op->err);
    r = SQLSetConnectAttr(op->cn->hdbc, SQL_ATTR_AUTOCOMMIT,
                          (SQLPOINTER)SQL_AUTOCOMMIT_OFF, SQL_IS_INTEGER);
    if (!SQL_SUCCEEDED(r)) {
        int q;
        diag_capture(op->err, SQL_HANDLE_DBC, op->cn->hdbc, "autocommit off", &q);
        op->ok = 0;
        return;
    }
    op->ok = 1;
}

static void job_commit(void* arg)
{
    OpSimple* op = (OpSimple*)arg;
    SQLRETURN r;
    vms_error_ok(op->err);
    r = SQLEndTran(SQL_HANDLE_DBC, op->cn->hdbc, SQL_COMMIT);
    if (!SQL_SUCCEEDED(r)) {
        int q;
        diag_capture(op->err, SQL_HANDLE_DBC, op->cn->hdbc, "commit", &q);
        if (q) InterlockedExchange(&op->cn->quarantined, 1);
        op->ok = 0;
        return;
    }
    op->ok = 1;
}

static void job_rollback(void* arg)
{
    OpSimple* op = (OpSimple*)arg;
    SQLRETURN r;
    vms_error_ok(op->err);
    r = SQLEndTran(SQL_HANDLE_DBC, op->cn->hdbc, SQL_ROLLBACK);
    if (!SQL_SUCCEEDED(r)) {
        int q;
        diag_capture(op->err, SQL_HANDLE_DBC, op->cn->hdbc, "rollback", &q);
        op->ok = 0;
        return;
    }
    op->ok = 1;
}

/* ================= public API ================= */

VmsClient* vms_client_init(VmsError* err)
{
    VmsClient* c;
    SQLHENV env = SQL_NULL_HENV;
    vms_error_ok(err);
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env))) {
        vms_error_set(err, VMS_ERR_INTERNAL, NULL, 0, "cannot allocate ODBC environment");
        return NULL;
    }
    if (!SQL_SUCCEEDED(SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION,
                                     (SQLPOINTER)SQL_OV_ODBC3_80, 0))) {
        vms_error_set(err, VMS_ERR_INTERNAL, NULL, 0, "cannot set ODBC 3.8");
        SQLFreeHandle(SQL_HANDLE_ENV, env);
        return NULL;
    }
    c = (VmsClient*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(VmsClient));
    if (!c) {
        SQLFreeHandle(SQL_HANDLE_ENV, env);
        vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM client");
        return NULL;
    }
    c->env = env;
    return c;
}

void vms_client_destroy(VmsClient* c)
{
    if (!c) return;
    if (c->env != SQL_NULL_HENV) SQLFreeHandle(SQL_HANDLE_ENV, c->env);
    HeapFree(GetProcessHeap(), 0, c);
}

VmsConnection* vms_conn_open(VmsClient* c, const wchar_t* connstr_w, VmsError* err)
{
    VmsConnection* cn;
    OpOpen op;
    vms_error_ok(err);
    if (!c || !connstr_w) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "bad args");
        return NULL;
    }
    cn = (VmsConnection*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(VmsConnection));
    if (!cn) {
        vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM connection");
        return NULL;
    }
    cn->client = c;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, c->env, &cn->hdbc))) {
        vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM hdbc");
        HeapFree(GetProcessHeap(), 0, cn);
        return NULL;
    }
    cn->worker = vms_worker_start();
    if (!cn->worker) {
        vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM worker");
        SQLFreeHandle(SQL_HANDLE_DBC, cn->hdbc);
        HeapFree(GetProcessHeap(), 0, cn);
        return NULL;
    }
    op.cn = cn;
    op.connstr = connstr_w;
    op.err = err;
    op.ok = 0;
    vms_worker_run(cn->worker, job_open, &op);
    if (!op.ok) {
        vms_worker_stop(cn->worker);
        SQLFreeHandle(SQL_HANDLE_DBC, cn->hdbc);
        HeapFree(GetProcessHeap(), 0, cn);
        return NULL;
    }
    /* manual-transaction mode is opt-in via vms_tran_begin(); default
     * connection stays in autocommit (SQLite drives transaction timing) */
    return cn;
}

void vms_conn_close(VmsConnection* cn)
{
    if (!cn) return;
    if (cn->worker) vms_worker_stop(cn->worker);
    if (cn->hdbc) {
        SQLDisconnect(cn->hdbc);
        SQLFreeHandle(SQL_HANDLE_DBC, cn->hdbc);
    }
    HeapFree(GetProcessHeap(), 0, cn);
}

int vms_conn_quarantined(const VmsConnection* cn)
{
    return cn ? (int)InterlockedCompareExchange((volatile LONG*)&cn->quarantined, 0, 0) : 0;
}

static VmsStatement* stmt_alloc(VmsConnection* cn)
{
    VmsStatement* st = (VmsStatement*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                sizeof(VmsStatement));
    if (!st) return NULL;
    st->conn = cn;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, cn->hdbc, &st->hstmt))) {
        HeapFree(GetProcessHeap(), 0, st);
        return NULL;
    }
    return st;
}

VmsStatement* vms_stmt_exec_direct(VmsConnection* cn, const wchar_t* sql_w,
                                   VmsError* err)
{
    VmsStatement* st;
    OpExec op;
    OpMeta meta;
    vms_error_ok(err);
    if (!cn || !sql_w) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "bad args");
        return NULL;
    }
    st = stmt_alloc(cn);
    if (!st) {
        vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM statement");
        return NULL;
    }
    op.cn = cn;
    op.st = st;
    op.sql = sql_w;
    op.err = err;
    vms_worker_run(cn->worker, job_exec, &op);
    if (!op.ok) {
        SQLFreeHandle(SQL_HANDLE_STMT, st->hstmt);
        HeapFree(GetProcessHeap(), 0, st);
        return NULL;
    }
    meta.st = st;
    meta.err = err;
    vms_worker_run(cn->worker, job_meta, &meta);
    if (!meta.ok) {
        SQLFreeHandle(SQL_HANDLE_STMT, st->hstmt);
        HeapFree(GetProcessHeap(), 0, st);
        return NULL;
    }
    return st;
}

VmsStatement* vms_stmt_exec_params(VmsConnection* cn, const wchar_t* sql_w,
                                   const VmsValue* params, int nparams,
                                   VmsError* err)
{
    /* R3 scope: parameterless exec is the workhorse; typed param binding
     * lands with the planner (R7). Refuse cleanly rather than half-work. */
    (void)cn; (void)sql_w; (void)params; (void)nparams;
    vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0,
                  "typed parameter binding arrives with the pushdown planner (R7); "
                  "use exec_direct or build literals server-side");
    return NULL;
}

void vms_stmt_destroy(VmsStatement* st)
{
    if (!st) return;
    row_clear(st);
    if (st->meta) HeapFree(GetProcessHeap(), 0, st->meta);
    if (st->hstmt) SQLFreeHandle(SQL_HANDLE_STMT, st->hstmt);
    HeapFree(GetProcessHeap(), 0, st);
}

int vms_stmt_fetch(VmsStatement* st, VmsError* err)
{
    OpFetch op;
    if (!st) return -1;
    if (InterlockedCompareExchange(&st->conn->quarantined, 0, 0)) {
        vms_error_set(err, VMS_ERR_QUARANTINED, NULL, 0, "connection quarantined");
        return -1;
    }
    op.st = st;
    op.err = err;
    vms_error_ok(err);
    vms_worker_run(st->conn->worker, job_fetch, &op);
    return op.result;
}

int vms_stmt_more_results(VmsStatement* st, VmsError* err)
{
    OpMore mr;
    if (!st) return -1;
    mr.st = st;
    mr.err = err;
    vms_worker_run(st->conn->worker, job_more, &mr);
    return mr.result;
}

int vms_stmt_col_count(const VmsStatement* st)
{
    return st ? st->col_count : 0;
}

const VmsColumnMeta* vms_stmt_meta(const VmsStatement* st, int col)
{
    if (!st || col < 0 || col >= st->col_count) return NULL;
    return &st->meta[col];
}

const VmsValue* vms_stmt_value(const VmsStatement* st, int col)
{
    if (!st || !st->row_ready || col < 0 || col >= st->col_count) return NULL;
    return &st->row[col];
}

int vms_stmt_cancel(VmsStatement* st)
{
    if (!st) return 0;
    vms_worker_cancel_active(st->conn->worker);
    return 1;
}

typedef struct OpVerify {
    VmsConnection* cn;
    int ok;
} OpVerify;

static void job_verify(void* arg)
{
    OpVerify* op = (OpVerify*)arg;
    SQLHSTMT st = SQL_NULL_HSTMT;
    SQLRETURN r;
    SQLBIGINT trancount = -1;
    SQLLEN ind = 0;
    SQLWCHAR q1[] = L"SELECT @@TRANCOUNT";
    SQLWCHAR q2[] = L"SELECT 1";

    op->ok = 0;
    if (InterlockedCompareExchange(&op->cn->quarantined, 0, 0)) return;

    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, op->cn->hdbc, &st))) return;
    r = SQLExecDirectW(st, q1, SQL_NTS);
    if (!SQL_SUCCEEDED(r)) { SQLFreeHandle(SQL_HANDLE_STMT, st); return; }
    if (!SQL_SUCCEEDED(SQLFetch(st)) ||
        !SQL_SUCCEEDED(SQLGetData(st, 1, SQL_C_SBIGINT, &trancount, 0, &ind)) ||
        trancount != 0) {
        SQLFreeHandle(SQL_HANDLE_STMT, st);
        return; /* open transaction: not clean */
    }
    SQLFreeStmt(st, SQL_CLOSE);
    r = SQLExecDirectW(st, q2, SQL_NTS);
    SQLFreeStmt(st, SQL_CLOSE);
    if (!SQL_SUCCEEDED(r)) { SQLFreeHandle(SQL_HANDLE_STMT, st); return; }
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    op->ok = 1;
}

int vms_conn_verify(VmsConnection* cn)
{
    OpVerify op;
    if (!cn) return 0;
    op.cn = cn;
    op.ok = 0;
    vms_worker_run(cn->worker, job_verify, &op);
    return op.ok;
}

int vms_conn_cancel(VmsConnection* cn)
{
    if (!cn) return 0;
    vms_worker_cancel_active(cn->worker);
    return 1;
}

int vms_tran_begin(VmsConnection* cn, VmsError* err)
{
    OpSimple op;
    if (!cn) return -1;
    op.cn = cn;
    op.err = err;
    op.ok = 0;
    vms_worker_run(cn->worker, job_begin, &op);
    return op.ok ? 0 : -1;
}

int vms_tran_commit(VmsConnection* cn, VmsError* err)
{
    OpSimple op;
    if (!cn) return -1;
    op.cn = cn;
    op.err = err;
    op.ok = 0;
    vms_worker_run(cn->worker, job_commit, &op);
    return op.ok ? 0 : -1;
}

int vms_tran_rollback(VmsConnection* cn, VmsError* err)
{
    OpSimple op;
    if (!cn) return -1;
    op.cn = cn;
    op.err = err;
    op.ok = 0;
    vms_worker_run(cn->worker, job_rollback, &op);
    return op.ok ? 0 : -1;
}
