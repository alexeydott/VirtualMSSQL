/* vms_odbc_adapter.c — ODBC client runtime (R3).
 *
 * ODBC types are confined to this file per the TZ layering rule.
 * Handles ownership: VmsConnection owns SQLHDBC, VmsStatement owns SQLHSTMT,
 * all calls run on the connection's worker; cancel goes through the worker's
 * published active statement (R0-proven scheme).
 * Rows become visible only after complete decode of every column. */
#include "vms_client.h"
#include "vms_meta.h"
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
    volatile LONG txn_active;   /* R11: remote transaction open */
    volatile LONG txn_pinned;   /* R11: manual-commit mode + primer applied */
    wchar_t* last_connstr;  /* kept for cursor leases (R6) */
};

struct VmsStatement {
    VmsConnection* conn;
    HSTMT hstmt;            /* single owner: this object */
    int col_count;
    VmsColumnMeta* meta;
    VmsValue* row;
    int row_ready;
};

/* R6 read cursor: independent lease = its own HDBC + worker, so nested
 * SQLite scans stream in parallel over their own connections. */
struct VmsCursor {
    HDBC hdbc;              /* owned lease, or the parent's HDBC (shared) */
    VmsWorker* worker;
    HSTMT hstmt;            /* single owner: this object */
    volatile LONG closed;
    int own_hdbc;           /* 1 = disconnect hdbc at close; 0 = shared */
    int col_count;
    VmsColumnMeta* meta;
    VmsValue* row;
    int row_ready;
    /* R7 pushdown parameter storage (deferred binding targets) */
    long long* param_vals;
    SQLLEN* param_inds;
    int nparams;
};

/* ================= value lifecycle ================= */

static void value_clear(VmsValue* v)
{
    if (v->text) { free(v->text); v->text = NULL; }
    if (v->blob) { free(v->blob); v->blob = NULL; }
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
        SQLWCHAR name[256];
        SQLSMALLINT name_len = 0, nullable = 0, digits = 0, sql_type = 0;
        SQLULEN col_size = 0;
        char name_u8[256];
        SQLRETURN dr = SQLDescribeColW(op->st->hstmt, (SQLUSMALLINT)i, name,
                                       (SQLSMALLINT)(sizeof(name) / sizeof(name[0])),
                                       &name_len, &sql_type, &col_size,
                                       &digits, &nullable);
        if (!SQL_SUCCEEDED(dr)) {
            int q;
            diag_capture(op->err, SQL_HANDLE_STMT, op->st->hstmt, "SQLDescribeColW", &q);
            op->ok = 0;
            return;
        }
        name[255] = 0; /* defensive terminator */
        if (name_len < 0 || name_len > 255) name_len = 255;
        name[name_len] = 0;
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

/* read a short driver-convertible scalar (decimal/datetime/guid) as WCHAR
 * text in one call — the driver renders these as bounded strings */
static int getdata_scalar_text(HSTMT h, int col, VmsValue* out, VmsError* err)
{
    wchar_t buf[256];
    SQLLEN got = 0;
    SQLRETURN r;
    size_t bytes8 = 0;
    int n;

    r = SQLGetData(h, (SQLUSMALLINT)col, SQL_C_WCHAR, buf, sizeof(buf), &got);
    if (r == SQL_NO_DATA || (r == SQL_SUCCESS && got == 0)) {
        out->type = VMS_VAL_TEXT;
        out->text = (char*)malloc(1);
        if (!out->text) return 0;
        out->text[0] = 0;
        out->text_len = 0;
        return 1;
    }
    if (!(r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO)) {
        int q;
        diag_capture(err, SQL_HANDLE_STMT, h, "SQLGetData(scalar text)", &q);
        return 0;
    }
    if (got == SQL_NULL_DATA) {
        out->type = VMS_VAL_NULL;
        return 1;
    }
    n = (got == SQL_NO_TOTAL || got < 0)
        ? (int)(sizeof(buf) / sizeof(wchar_t)) - 1
        : (int)(got / sizeof(wchar_t));
    if (n < 0) n = 0;
    if (vms_utf16_to_utf8(buf, (size_t)n, NULL, 0, &bytes8) != 0) {
        vms_error_set(err, VMS_ERR_INTERNAL, NULL, 0, "invalid utf16 scalar");
        return 0;
    }
    out->text = (char*)malloc(bytes8 + 1);
    if (!out->text) {
        vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM scalar text");
        return 0;
    }
    vms_utf16_to_utf8(buf, (size_t)n, out->text, bytes8 + 1, &bytes8);
    out->text[bytes8] = 0;
    out->text_len = bytes8;
    out->type = VMS_VAL_TEXT;
    return 1;
}

/* read one text column in bounded chunks as raw UTF-16LE bytes, then convert
 * the complete value once (strict validation over the whole string: chunk
 * boundaries may split surrogate pairs, so per-chunk strict conversion is
 * not possible). Ordinals still increase monotonically (1..n). */
static int getdata_text(HSTMT h, int col, VmsValue* out, VmsError* err)
{
    unsigned char chunk[VMS_GETDATA_CHUNK];
    unsigned char* acc = NULL;
    size_t acc_len = 0, acc_cap = 0;
    SQLLEN got = 0;
    SQLRETURN r;
    size_t wchars = 0;

    for (;;) {
        r = SQLGetData(h, (SQLUSMALLINT)col, SQL_C_BINARY, chunk, sizeof(chunk), &got);
        if (r == SQL_NO_DATA) break;
        if (!(r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO)) {
            int q;
            diag_capture(err, SQL_HANDLE_STMT, h, "SQLGetData(text)", &q);
            free(acc);
            return 0;
        }
        if (got == SQL_NULL_DATA) {
            out->type = VMS_VAL_NULL;
            free(acc);
            return 1;
        }
        if (got > 0) {
            size_t bytes = (size_t)got;
            if (bytes > sizeof(chunk)) bytes = sizeof(chunk); /* got may exceed BufferLength */
            if (acc_len + bytes + 1 > acc_cap) {
                size_t ncap = acc_cap ? acc_cap * 2 : 65536;
                unsigned char* na;
                while (ncap < acc_len + bytes + 1) ncap *= 2;
                na = (unsigned char*)realloc(acc, ncap);
                if (!na) {
                    vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM row buffer");
                    free(acc);
                    return 0;
                }
                acc = na;
                acc_cap = ncap;
            }
            memcpy(acc + acc_len, chunk, bytes);
            acc_len += bytes;
            acc[acc_len] = 0;
            wchars += bytes / sizeof(wchar_t);
        }
        if (r == SQL_SUCCESS) break; /* last chunk */
    }
    /* convert the assembled UTF-16LE value to UTF-8 in one strict pass */
    {
        size_t bytes8 = 0;
        if (wchars == 0) {
            out->type = VMS_VAL_TEXT;
            out->text = (char*)malloc(1);
            if (out->text) out->text[0] = 0;
            out->text_len = 0;
            free(acc);
            return out->text ? 1 : 0;
        }
        if (acc_len % 2 != 0) {
            vms_error_set(err, VMS_ERR_INTERNAL, NULL, 0, "odd utf16 byte count");
            free(acc);
            return 0;
        }
        /* UTF-8 worst case: 3 bytes per UTF-16 code unit (no 4-byte output
         * from a single UTF-16 unit; surrogates combine into <= 4 bytes but
         * a pair consumes 2 units, so 3x is a safe upper bound). */
        bytes8 = wchars * 3 + 1;
        out->text = (char*)malloc(bytes8);
        if (!out->text) {
            vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM convert out");
            free(acc);
            return 0;
        }
        if (vms_utf16_to_utf8((const wchar_t*)acc, wchars, out->text, bytes8, &bytes8) != 0) {
            vms_error_set(err, VMS_ERR_INTERNAL, NULL, 0, "invalid utf16 in column");
            free(out->text);
            out->text = NULL;
            free(acc);
            return 0;
        }
        out->text[bytes8] = 0;
        out->text_len = bytes8;
        out->type = VMS_VAL_TEXT;
    }
    free(acc);
    return 1;
}

static int getdata_blob(HSTMT h, int col, VmsValue* out, VmsError* err)
{
    unsigned char chunk[VMS_GETDATA_CHUNK];
    unsigned char* acc = NULL;
    size_t acc_len = 0, acc_cap = 0;
    SQLLEN got = 0;
    SQLRETURN r;

    for (;;) {
        r = SQLGetData(h, (SQLUSMALLINT)col, SQL_C_BINARY, chunk, sizeof(chunk), &got);
        if (r == SQL_NO_DATA) break;
        if (!(r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO)) {
            int q;
            diag_capture(err, SQL_HANDLE_STMT, h, "SQLGetData(blob)", &q);
            free(acc);
            return 0;
        }
        if (got == SQL_NULL_DATA) {
            out->type = VMS_VAL_NULL;
            free(acc);
            return 1;
        }
        if (got > 0) {
            size_t bytes = (size_t)got;
            if (bytes > sizeof(chunk)) bytes = sizeof(chunk); /* got may exceed BufferLength */
            if (acc_len + bytes + 1 > acc_cap) {
                size_t ncap = acc_cap ? acc_cap * 2 : 65536;
                unsigned char* na;
                while (ncap < acc_len + bytes + 1) ncap *= 2;
                na = (unsigned char*)realloc(acc, ncap);
                if (!na) {
                    vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM row buffer");
                    free(acc);
                    return 0;
                }
                acc = na;
                acc_cap = ncap;
            }
            memcpy(acc + acc_len, chunk, bytes);
            acc_len += bytes;
            acc[acc_len] = 0;
        }
        if (r == SQL_SUCCESS) break;
    }
    out->type = VMS_VAL_BLOB;
    out->blob_len = acc_len;
    out->blob = acc;
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
        st->row = (VmsValue*)calloc(1, sizeof(VmsValue) * (size_t)st->col_count);
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
        case VMS_CT_TEXT: case VMS_CT_BIGTEXT:
            if (!getdata_text(st->hstmt, i, v, op->err)) { op->result = -1; return; }
            break;
        case VMS_CT_DECIMAL: case VMS_CT_DATETIME: case VMS_CT_GUID:
            if (!getdata_scalar_text(st->hstmt, i, v, op->err)) { op->result = -1; return; }
            break;
        case VMS_CT_SPATIAL:
            /* R12: spatial UDTs stream as WKB through SQLGetData binary
             * (the native CLR serialization is not exposed to ODBC) */
            if (!getdata_blob(st->hstmt, i, v, op->err)) { op->result = -1; return; }
            break;
        case VMS_CT_BLOB:
            if (!getdata_blob(st->hstmt, i, v, op->err)) { op->result = -1; return; }
            break;
        case VMS_CT_UNSUPPORTED:
            /* deterministic policy: the column never yields a value */
            vms_error_set(op->err, VMS_ERR_UNSUPPORTED_TYPE, "IM001", 0,
                          "UNSUPPORTED_TYPE: column '%s' has a type with no "
                          "lossless mapping", m->name);
            op->result = -1;
            return;
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
    /* remember the connstr for cursor leases (R6) */
    {
        size_t n = wcslen(connstr_w) + 1;
        cn->last_connstr = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, n * sizeof(wchar_t));
        if (cn->last_connstr) memcpy(cn->last_connstr, connstr_w, n * sizeof(wchar_t));
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
    if (cn->last_connstr) {
        HeapFree(GetProcessHeap(), 0, cn->last_connstr);
        cn->last_connstr = NULL;
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

/* ---- R10 DML execution (adapter-side; values are bound, never inline) ---- */

typedef struct OpDml {
    VmsConnection* cn;
    const wchar_t* sql;
    const long long* iparams;
    int nint;
    const wchar_t* const* tparams;
    const int* tlengths;
    int ntext;
    const int* torder;
    int ntotal;
    long long rows;
    VmsError* err;
    int ok;
} OpDml;

static void job_dml(void* arg)
{
    OpDml* op = (OpDml*)arg;
    SQLHSTMT h = SQL_NULL_HSTMT;
    SQLRETURN r;
    SQLLEN rows = 0;
    long long* ival_holders = NULL;
    wchar_t** tval_holders = NULL;
    SQLLEN* inds = NULL;
    int i;

    vms_error_ok(op->err);
    if (InterlockedCompareExchange(&op->cn->quarantined, 0, 0)) {
        vms_error_set(op->err, VMS_ERR_QUARANTINED, NULL, 0, "connection quarantined");
        return;
    }
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, op->cn->hdbc, &h))) {
        vms_error_set(op->err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM dml stmt");
        return;
    }
    /* holders live until after SQLExecDirect (deferred binding) */
    if (op->nint > 0) {
        ival_holders = (long long*)HeapAlloc(GetProcessHeap(), 0,
                                             sizeof(long long) * (size_t)op->nint);
        inds = (SQLLEN*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                  sizeof(SQLLEN) * (size_t)op->ntotal);
        if (!ival_holders || !inds) goto fail_oom;
    }
    if (op->ntext > 0) {
        tval_holders = (wchar_t**)HeapAlloc(GetProcessHeap(), 0,
                                            sizeof(wchar_t*) * (size_t)op->ntext);
        if (!tval_holders) goto fail_oom;
        for (i = 0; i < op->ntext; i++) {
            int chars = op->tlengths ? op->tlengths[i] : 0;
            tval_holders[i] = (wchar_t*)HeapAlloc(GetProcessHeap(), 0,
                (size_t)(chars + 1) * sizeof(wchar_t));
            if (!tval_holders[i]) goto fail_oom;
            memcpy(tval_holders[i], op->tparams[i],
                   (size_t)chars * sizeof(wchar_t));
            tval_holders[i][chars] = 0;
        }
    }

    /* bind in statement order per torder */
    for (i = 0; i < op->ntotal; i++) {
        SQLRETURN br;
        int t = op->torder[i];
        if (t >= 0) {
            /* int param t */
            SQLBIGINT v = (SQLBIGINT)op->iparams[t];
            ival_holders[t] = v;
            inds[i] = sizeof(SQLBIGINT); /* fixed-length: size, not NULL */
            br = SQLBindParameter(h, (SQLUSMALLINT)(i + 1), SQL_PARAM_INPUT,
                                  SQL_C_SBIGINT, SQL_BIGINT, 0, 0,
                                  (SQLPOINTER)&ival_holders[t], 0, &inds[i]);
        } else {
            /* text param -(t)-1 */
            int ti = -t - 1;
            SQLULEN unlimited = 1073741823; /* R0: max accepted precision */
            wchar_t* hv = tval_holders[ti];
            int chars = op->tlengths ? op->tlengths[ti] : (int)wcslen(hv);
            if (chars == 0) {
                /* empty string bound as NULL when the caller marked it so */
                inds[i] = SQL_NULL_DATA;
                br = SQLBindParameter(h, (SQLUSMALLINT)(i + 1), SQL_PARAM_INPUT,
                                      SQL_C_WCHAR, SQL_WVARCHAR, 1, 0,
                                      (SQLPOINTER)L"", 0, &inds[i]);
            } else {
                inds[i] = (SQLLEN)(chars * sizeof(wchar_t));
                br = SQLBindParameter(h, (SQLUSMALLINT)(i + 1), SQL_PARAM_INPUT,
                                      SQL_C_WCHAR, SQL_WLONGVARCHAR, unlimited, 0,
                                      hv, 0, &inds[i]);
            }
        }
        if (!SQL_SUCCEEDED(br)) {
            int q;
            diag_capture(op->err, SQL_HANDLE_STMT, h, "SQLBindParameter", &q);
            goto fail;
        }
    }

    vms_worker_set_active(op->cn->worker, h);
    r = SQLExecDirectW(h, (SQLWCHAR*)op->sql, SQL_NTS);
    vms_worker_set_active(op->cn->worker, NULL);
    if (!(r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO || r == SQL_NO_DATA)) {
        int q;
        diag_capture(op->err, SQL_HANDLE_STMT, h, "DML execute", &q);
        goto fail;
    }
    if (!SQL_SUCCEEDED(SQLRowCount(h, &rows))) rows = -1;
    SQLFreeStmt(h, SQL_CLOSE);
    SQLFreeHandle(SQL_HANDLE_STMT, h);
    /* free holders */
    if (ival_holders) HeapFree(GetProcessHeap(), 0, ival_holders);
    if (inds) HeapFree(GetProcessHeap(), 0, inds);
    if (tval_holders) {
        for (i = 0; i < op->ntext; i++)
            if (tval_holders[i]) HeapFree(GetProcessHeap(), 0, tval_holders[i]);
        HeapFree(GetProcessHeap(), 0, tval_holders);
    }
    op->rows = (long long)rows;
    /* NOTE (R11): the defensive auto-commit that used to live here
     * ("IF @@TRANCOUNT > 0 COMMIT TRAN") is removed — inside an explicit
     * transaction it silently committed the remote work after every DML,
     * destroying savepoints. Transaction scope is owned exclusively by the
     * R11 pin/lazy-BEGIN/finalize machinery. */
    op->ok = 1;
    return;

fail_oom:
    vms_error_set(op->err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM dml holders");
fail:
    if (h != SQL_NULL_HSTMT) {
        SQLFreeStmt(h, SQL_CLOSE);
        SQLFreeHandle(SQL_HANDLE_STMT, h);
    }
    if (ival_holders) HeapFree(GetProcessHeap(), 0, ival_holders);
    if (inds) HeapFree(GetProcessHeap(), 0, inds);
    if (tval_holders) {
        for (i = 0; i < op->ntext; i++)
            if (tval_holders[i]) HeapFree(GetProcessHeap(), 0, tval_holders[i]);
        HeapFree(GetProcessHeap(), 0, tval_holders);
    }
    op->ok = 0;
}

int vms_conn_exec_dml(VmsConnection* cn, const wchar_t* sql,
                      const long long* iparams, int nint,
                      const wchar_t* const* tparams, const int* tlengths, int ntext,
                      const int* torder, int ntotal,
                      long long* rows_affected, VmsError* err)
{
    OpDml op;
    if (!cn || !sql) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "exec_dml: bad args");
        return -1;
    }
    op.cn = cn;
    op.sql = sql;
    op.iparams = iparams;
    op.nint = nint;
    op.tparams = tparams;
    op.tlengths = tlengths;
    op.ntext = ntext;
    op.torder = torder;
    op.ntotal = ntotal;
    op.rows = -1;
    op.err = err;
    op.ok = 0;
    vms_error_ok(err);
    vms_worker_run(cn->worker, job_dml, &op);
    if (!op.ok) return -1;
    if (rows_affected) *rows_affected = op.rows;
    return 0;
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

/* ================= R11 explicit transactions ================= */

/* SQL_COPT_SS_AUTOBEGINTXN (msodbcsql.h): 1400 + 2 */
#ifndef SQL_COPT_SS_AUTOBEGINTXN
#define SQL_COPT_SS_AUTOBEGINTXN 1402
#endif
#ifndef SQL_AUTOBEGINTXN_OFF
#define SQL_AUTOBEGINTXN_OFF 0UL
#endif

typedef struct OpTxn {
    VmsConnection* cn;
    const char* name;       /* savepoint name (validated identifier) */
    int rollback_op;        /* savepoint: 0 = SAVE, 1 = ROLLBACK TO */
    VmsTxnResult result;
    VmsError* err;
    int ok;
} OpTxn;

/* one-shot exec of a UTF-8 statement on the connection (worker context) */
static SQLRETURN txn_exec_utf8(HDBC hdbc, const char* sql_utf8)
{
    SQLHSTMT st = SQL_NULL_HSTMT;
    SQLRETURN r;
    wchar_t wsql[512];
    int n = MultiByteToWideChar(CP_UTF8, 0, sql_utf8, -1, wsql, 512);
    if (n <= 0) return SQL_ERROR;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &st))) return SQL_ERROR;
    r = SQLExecDirectW(st, wsql, SQL_NTS);
    SQLFreeStmt(st, SQL_CLOSE);
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    return r;
}

/* pin: manual-commit mode + AUTOBEGINTXN OFF + primer. Runs on the worker. */
static void job_txn_pin(void* arg)
{
    OpTxn* op = (OpTxn*)arg;
    SQLRETURN r;
    vms_error_ok(op->err);
    if (InterlockedCompareExchange(&op->cn->txn_pinned, 0, 0)) { op->ok = 1; return; }

    /* manual-commit mode: SQLEndTran drives finalization */
    r = SQLSetConnectAttr(op->cn->hdbc, SQL_ATTR_AUTOCOMMIT,
                          (SQLPOINTER)SQL_AUTOCOMMIT_OFF, SQL_IS_INTEGER);
    if (!SQL_SUCCEEDED(r)) {
        int q;
        diag_capture(op->err, SQL_HANDLE_DBC, op->cn->hdbc, "txn pin: autocommit off", &q);
        op->ok = 0;
        return;
    }
    /* the driver must not auto-begin: BEGIN is issued explicitly (lazy) */
    r = SQLSetConnectAttr(op->cn->hdbc, SQL_COPT_SS_AUTOBEGINTXN,
                          (SQLPOINTER)SQL_AUTOBEGINTXN_OFF, SQL_IS_INTEGER);
    if (!SQL_SUCCEEDED(r)) {
        int q;
        diag_capture(op->err, SQL_HANDLE_DBC, op->cn->hdbc, "txn pin: autobegin off", &q);
        op->ok = 0;
        return;
    }
    /* primer: XACT_ABORT ON makes runtime errors doom the transaction, so
     * xSync can detect the doomed state via XACT_STATE(). */
    if (!SQL_SUCCEEDED(txn_exec_utf8(op->cn->hdbc, "SET XACT_ABORT ON"))) {
        int q;
        diag_capture(op->err, SQL_HANDLE_DBC, op->cn->hdbc, "txn pin: primer", &q);
        /* restore autocommit before giving the connection back */
        SQLSetConnectAttr(op->cn->hdbc, SQL_ATTR_AUTOCOMMIT,
                          (SQLPOINTER)SQL_AUTOCOMMIT_ON, SQL_IS_INTEGER);
        op->ok = 0;
        return;
    }
    InterlockedExchange(&op->cn->txn_pinned, 1);
    op->ok = 1;
}

/* lazy BEGIN: explicitly open the server transaction when the first write
 * (or savepoint) joins. Idempotent: skipped once the transaction is open.
 * SQLEndTran(COMMIT) maps to one COMMIT TRANSACTION, which matches this
 * exactly one BEGIN (TRANCOUNT 1 -> 0). */
static void job_txn_begin_lazy(void* arg)
{
    OpTxn* op = (OpTxn*)arg;
    SQLHSTMT st = SQL_NULL_HSTMT;
    SQLRETURN r;
    SQLBIGINT tc = -1;
    SQLLEN ind = 0;
    SQLWCHAR q1[] = L"SELECT @@TRANCOUNT";
    vms_error_ok(op->err);
    if (InterlockedCompareExchange(&op->cn->txn_active, 0, 0)) { op->ok = 1; return; }
    if (!InterlockedCompareExchange(&op->cn->txn_pinned, 0, 0)) {
        vms_error_set(op->err, VMS_ERR_INVALID_ARG, NULL, 0,
                      "lazy txn start: connection not pinned");
        op->ok = 0;
        return;
    }
    if (SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, op->cn->hdbc, &st)) &&
        SQL_SUCCEEDED(SQLExecDirectW(st, q1, SQL_NTS)) &&
        SQL_SUCCEEDED(SQLFetch(st)) &&
        SQL_SUCCEEDED(SQLGetData(st, 1, SQL_C_SBIGINT, &tc, 0, &ind))) {
        SQLFreeStmt(st, SQL_CLOSE);
        SQLFreeHandle(SQL_HANDLE_STMT, st);
        st = SQL_NULL_HSTMT;
        if (tc > 0) {
            /* driver-managed txn already open (autocommit=OFF implicit) */
            InterlockedExchange(&op->cn->txn_active, 1);
            op->ok = 1;
            return;
        }
        if (SQL_SUCCEEDED(txn_exec_utf8(op->cn->hdbc, "BEGIN TRANSACTION"))) {
            InterlockedExchange(&op->cn->txn_active, 1);
            op->ok = 1;
            return;
        }
    }
    if (st != SQL_NULL_HSTMT) {
        SQLFreeStmt(st, SQL_CLOSE);
        SQLFreeHandle(SQL_HANDLE_STMT, st);
    }
    op->ok = 0;
    op->err->cls = VMS_ERR_EXEC;
    _snprintf_s(op->err->message, sizeof(op->err->message), _TRUNCATE,
                "lazy txn start failed");
}

/* savepoint statement: SAVE / ROLLBACK TRANSACTION <name> */
static void job_txn_savepoint(void* arg)
{
    OpTxn* op = (OpTxn*)arg;
    char sql[160];
    vms_error_ok(op->err);
    _snprintf_s(sql, sizeof(sql), _TRUNCATE, "%s TRANSACTION [%s]",
                op->rollback_op ? "ROLLBACK" : "SAVE", op->name);
    {
        /* diag must be captured from the statement handle itself */
        SQLHSTMT st = SQL_NULL_HSTMT;
        SQLRETURN r;
        wchar_t wsql[512];
        MultiByteToWideChar(CP_UTF8, 0, sql, -1, wsql, 512);
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, op->cn->hdbc, &st))) {
            vms_error_set(op->err, VMS_ERR_INTERNAL, NULL, 0, "savepoint: alloc failed");
            op->ok = 0;
            return;
        }
        r = SQLExecDirectW(st, wsql, SQL_NTS);
        if (!SQL_SUCCEEDED(r)) {
            int q;
            SQLWCHAR state[6]; SQLWCHAR msg[1024];
            SQLINTEGER native = 0; SQLSMALLINT len = 0;
            char st_u8[8] = {0}; char msg_u8[512] = {0};
            SQLRETURN dr = SQLGetDiagRecW(SQL_HANDLE_STMT, st, 1, state, &native,
                                          msg, 1024, &len);
            (void)q;
            if (SQL_SUCCEEDED(dr)) {
                WideCharToMultiByte(CP_UTF8, 0, (LPCWCH)state, -1, st_u8, sizeof(st_u8), NULL, NULL);
                WideCharToMultiByte(CP_UTF8, 0, (LPCWCH)msg, -1, msg_u8, sizeof(msg_u8), NULL, NULL);
            }
            vms_error_set(op->err, VMS_ERR_EXEC, st_u8, (int)native,
                          "%s: %s", sql, msg_u8[0] ? msg_u8 : "no diagnostics");
            SQLFreeHandle(SQL_HANDLE_STMT, st);
            op->ok = 0;
            return;
        }
        SQLFreeStmt(st, SQL_CLOSE);
        SQLFreeHandle(SQL_HANDLE_STMT, st);
    }
    op->ok = 1;
}

/* XACT_STATE() == -1 → uncommittable (doomed) transaction */
static void job_txn_doomed(void* arg)
{
    OpTxn* op = (OpTxn*)arg;
    SQLHSTMT st = SQL_NULL_HSTMT;
    SQLRETURN r;
    SQLINTEGER state = 0;
    SQLLEN ind = 0;
    SQLWCHAR q[] = L"SELECT XACT_STATE()";
    op->ok = 0;
    if (InterlockedCompareExchange(&op->cn->txn_active, 0, 0)) {
        if (SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, op->cn->hdbc, &st))) {
            r = SQLExecDirectW(st, q, SQL_NTS);
            if (SQL_SUCCEEDED(r) && SQL_SUCCEEDED(SQLFetch(st)) &&
                SQL_SUCCEEDED(SQLGetData(st, 1, SQL_C_SLONG, &state, 0, &ind))) {
                op->ok = (state == -1);
            }
            SQLFreeStmt(st, SQL_CLOSE);
            SQLFreeHandle(SQL_HANDLE_STMT, st);
        }
    }
}

static void job_txn_finalize(void* arg)
{
    OpTxn* op = (OpTxn*)arg;
    SQLRETURN r;
    int was_active;
    vms_error_ok(op->err);

    /* non-cancellable: attention is pointless once finalization started */
    was_active = InterlockedExchange(&op->cn->txn_active, 0);
    r = SQLEndTran(SQL_HANDLE_DBC, op->cn->hdbc,
                   op->rollback_op ? SQL_ROLLBACK : SQL_COMMIT);
    if (!SQL_SUCCEEDED(r)) {
        int q;
        VmsErrClass cls = diag_capture(op->err, SQL_HANDLE_DBC, op->cn->hdbc,
                                       op->rollback_op ? "SQLEndTran rollback" : "SQLEndTran commit", &q);
        if (q || cls == VMS_ERR_TRANSPORT || cls == VMS_ERR_CONNECT) {
            /* wire-level failure during COMMIT: outcome unknown. The
             * connection may have committed before the failure — it must
             * never be reused. */
            InterlockedExchange(&op->cn->quarantined, 1);
            op->result = VMS_TXN_UNKNOWN;
            return;
        }
        if (cls == VMS_ERR_TIMEOUT) {
            /* lock timeout: retryable; the transaction is still open */
            if (was_active) InterlockedExchange(&op->cn->txn_active, 1);
            op->result = VMS_TXN_BUSY;
            return;
        }
        /* other driver errors: treat conservatively as unknown only when a
         * COMMIT was in flight; rollback failures leave no ambiguity */
        if (!op->rollback_op && was_active) {
            InterlockedExchange(&op->cn->quarantined, 1);
            op->result = VMS_TXN_UNKNOWN;
        } else {
            op->result = VMS_TXN_BUSY;
        }
        return;
    }
    /* restore autocommit so the connection can return to the pool */
    SQLSetConnectAttr(op->cn->hdbc, SQL_ATTR_AUTOCOMMIT,
                      (SQLPOINTER)SQL_AUTOCOMMIT_ON, SQL_IS_INTEGER);
    InterlockedExchange(&op->cn->txn_pinned, 0);
    op->result = VMS_TXN_OK;
}

/* ---- R11 public API ---- */

int vms_txn_pin(VmsConnection* cn, VmsError* err)
{
    OpTxn op;
    if (!cn) return -1;
    op.cn = cn; op.err = err; op.ok = 0; op.name = NULL; op.rollback_op = 0; op.result = VMS_TXN_UNKNOWN;
    vms_worker_run(cn->worker, job_txn_pin, &op);
    return op.ok ? 0 : -1;
}

int vms_txn_begin_lazy(VmsConnection* cn, VmsError* err)
{
    OpTxn op;
    if (!cn) return -1;
    op.cn = cn; op.err = err; op.ok = 0; op.name = NULL; op.rollback_op = 0; op.result = VMS_TXN_UNKNOWN;
    vms_worker_run(cn->worker, job_txn_begin_lazy, &op);
    return op.ok ? 0 : -1;
}

int vms_txn_active(VmsConnection* cn)
{
    return cn ? (int)InterlockedCompareExchange(&cn->txn_active, 0, 0) : 0;
}

int vms_txn_savepoint(VmsConnection* cn, const char* name, int rollback_op,
                      VmsError* err)
{
    OpTxn op;
    if (!cn || !name || !vms_meta_ident_valid(name, 128)) return -1;
    op.cn = cn; op.err = err; op.ok = 0;
    op.name = name; op.rollback_op = rollback_op; op.result = VMS_TXN_UNKNOWN;
    vms_worker_run(cn->worker, job_txn_savepoint, &op);
    return op.ok ? 0 : -1;
}

int vms_txn_doomed(const VmsConnection* cn)
{
    OpTxn op;
    VmsError scratch;
    if (!cn) return 0;
    op.cn = (VmsConnection*)cn; op.err = &scratch; op.ok = 0;
    op.name = NULL; op.rollback_op = 0; op.result = VMS_TXN_UNKNOWN;
    vms_worker_run(cn->worker, job_txn_doomed, &op);
    return op.ok;
}

int vms_txn_validate(const VmsConnection* cn)
{
    if (!vms_txn_active((VmsConnection*)cn)) return 0;
    return !vms_txn_doomed(cn);
}

VmsTxnResult vms_txn_commit(VmsConnection* cn, VmsError* err)
{
    OpTxn op;
    if (!cn) return VMS_TXN_UNKNOWN;
    op.cn = cn; op.err = err; op.ok = 0;
    op.name = NULL; op.rollback_op = 0; op.result = VMS_TXN_UNKNOWN;
    vms_worker_run(cn->worker, job_txn_finalize, &op);
    return op.result;
}

VmsTxnResult vms_txn_rollback(VmsConnection* cn, VmsError* err)
{
    OpTxn op;
    if (!cn) return VMS_TXN_UNKNOWN;
    op.cn = cn; op.err = err; op.ok = 0;
    op.name = NULL; op.rollback_op = 1; op.result = VMS_TXN_UNKNOWN;
    vms_worker_run(cn->worker, job_txn_finalize, &op);
    return op.result;
}

/* ================= R6 read cursor (independent lease) ================= */

/* cursor jobs operate on the lease's own worker; the fetch job mirrors
 * job_fetch but owns its HDBC/HSTMT directly */

typedef struct OpCursorOpen {
    VmsCursor* cur;
    const wchar_t* sql;
    VmsError* err;
    int ok;
} OpCursorOpen;

static void job_cursor_open(void* arg)
{
    OpCursorOpen* op = (OpCursorOpen*)arg;
    SQLRETURN r;
    vms_error_ok(op->err);
    vms_worker_set_active(op->cur->worker, op->cur->hstmt);
    r = SQLExecDirectW(op->cur->hstmt, (SQLWCHAR*)op->sql, SQL_NTS);
    vms_worker_set_active(op->cur->worker, NULL);
    if (!(r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO || r == SQL_NO_DATA)) {
        int q;
        diag_capture(op->err, SQL_HANDLE_STMT, op->cur->hstmt, "cursor open", &q);
        op->ok = 0;
        return;
    }
    op->ok = 1;
}

/* cursor meta: shared shape with OpMeta but on the cursor handle */
typedef struct OpCursorMeta {
    VmsCursor* cur;
    VmsError* err;
    int ok;
} OpCursorMeta;

static void job_cursor_meta(void* arg)
{
    OpCursorMeta* op = (OpCursorMeta*)arg;
    VmsCursor* cur = op->cur;
    SQLSMALLINT n = 0;
    int i;
    vms_error_ok(op->err);
    if (!SQL_SUCCEEDED(SQLNumResultCols(cur->hstmt, &n))) {
        int q;
        diag_capture(op->err, SQL_HANDLE_STMT, cur->hstmt, "cursor SQLNumResultCols", &q);
        op->ok = 0;
        return;
    }
    cur->col_count = n;
    if (n > 0) {
        cur->meta = (VmsColumnMeta*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                              sizeof(VmsColumnMeta) * (size_t)n);
        if (!cur->meta) {
            vms_error_set(op->err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM cursor meta");
            op->ok = 0;
            return;
        }
    }
    for (i = 1; i <= n; i++) {
        SQLWCHAR name[256];
        SQLSMALLINT name_len = 0, nullable = 0, digits = 0, sql_type = 0;
        SQLULEN col_size = 0;
        char name_u8[256];
        if (!SQL_SUCCEEDED(SQLDescribeColW(cur->hstmt, (SQLUSMALLINT)i, name,
                                           (SQLSMALLINT)(sizeof(name) / sizeof(name[0])),
                                           &name_len, &sql_type, &col_size,
                                           &digits, &nullable))) {
            int q;
            diag_capture(op->err, SQL_HANDLE_STMT, cur->hstmt, "cursor SQLDescribeColW", &q);
            op->ok = 0;
            return;
        }
        name[255] = 0;
        if (name_len < 0 || name_len > 255) name_len = 255;
        name[name_len] = 0;
        WideCharToMultiByte(CP_UTF8, 0, (LPCWCH)name, -1, name_u8, sizeof(name_u8), NULL, NULL);
        strncpy_s(cur->meta[i - 1].name, sizeof(cur->meta[i - 1].name), name_u8, _TRUNCATE);
        cur->meta[i - 1].type = map_col_type(sql_type, col_size);
        cur->meta[i - 1].nullable = (nullable != SQL_NO_NULLS);
        cur->meta[i - 1].col_size = (unsigned long)col_size;
        cur->meta[i - 1].decimal_digits = (unsigned short)digits;
    }
    op->ok = 1;
}

typedef struct OpCursorFetch {
    VmsCursor* cur;
    VmsError* err;
    int result;
} OpCursorFetch;

static void job_cursor_fetch(void* arg)
{
    OpCursorFetch* op = (OpCursorFetch*)arg;
    VmsCursor* cur = op->cur;
    SQLRETURN r;
    int i;

    vms_error_ok(op->err);
    if (InterlockedCompareExchange(&cur->closed, 0, 0)) {
        vms_error_set(op->err, VMS_ERR_QUARANTINED, NULL, 0, "cursor closed");
        op->result = -1;
        return;
    }
    /* release previous row before fetching the next */
    if (cur->row) {
        int j;
        for (j = 0; j < cur->col_count; j++) value_clear(&cur->row[j]);
        cur->row_ready = 0;
    }
    vms_worker_set_active(cur->worker, cur->hstmt);
    r = SQLFetch(cur->hstmt);
    vms_worker_set_active(cur->worker, NULL);
    if (r == SQL_NO_DATA) { op->result = 0; return; }
    if (!(r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO)) {
        int q;
        diag_capture(op->err, SQL_HANDLE_STMT, cur->hstmt, "cursor SQLFetch", &q);
        op->result = -1;
        return;
    }
    if (!cur->row && cur->col_count > 0) {
        cur->row = (VmsValue*)calloc(1, sizeof(VmsValue) * (size_t)cur->col_count);
        if (!cur->row) {
            vms_error_set(op->err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM cursor row");
            op->result = -1;
            return;
        }
    }
    /* complete-row decode before visibility (TZ invariant) */
    for (i = 1; i <= cur->col_count; i++) {
        VmsValue* v = &cur->row[i - 1];
        const VmsColumnMeta* m = &cur->meta[i - 1];
        switch (m->type) {
        case VMS_CT_INT64: {
            SQLBIGINT iv = 0;
            SQLLEN ind = 0;
            if (!SQL_SUCCEEDED(SQLGetData(cur->hstmt, (SQLUSMALLINT)i, SQL_C_SBIGINT,
                                          &iv, 0, &ind))) {
                int q;
                diag_capture(op->err, SQL_HANDLE_STMT, cur->hstmt, "cursor SQLGetData(int)", &q);
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
            if (!SQL_SUCCEEDED(SQLGetData(cur->hstmt, (SQLUSMALLINT)i, SQL_C_DOUBLE,
                                          &dv, 0, &ind))) {
                int q;
                diag_capture(op->err, SQL_HANDLE_STMT, cur->hstmt, "cursor SQLGetData(float)", &q);
                op->result = -1;
                return;
            }
            if (ind == SQL_NULL_DATA) v->type = VMS_VAL_NULL;
            else { v->type = VMS_VAL_FLOAT64; v->f = dv; }
            break;
        }
        case VMS_CT_TEXT: case VMS_CT_BIGTEXT:
            if (!getdata_text(cur->hstmt, i, v, op->err)) { op->result = -1; return; }
            break;
        case VMS_CT_DECIMAL: case VMS_CT_DATETIME: case VMS_CT_GUID:
            /* driver-convertible scalars: fetch as driver text (WCHAR) */
            if (!getdata_scalar_text(cur->hstmt, i, v, op->err)) { op->result = -1; return; }
            break;
        case VMS_CT_SPATIAL:
            /* R12: spatial UDTs stream as WKB through SQLGetData binary */
            if (!getdata_blob(cur->hstmt, i, v, op->err)) { op->result = -1; return; }
            break;
        case VMS_CT_BLOB:
            if (!getdata_blob(cur->hstmt, i, v, op->err)) { op->result = -1; return; }
            break;
        case VMS_CT_UNSUPPORTED:
            /* deterministic policy: the column never yields a value */
            vms_error_set(op->err, VMS_ERR_UNSUPPORTED_TYPE, "IM001", 0,
                          "UNSUPPORTED_TYPE: column '%s' has a type with no "
                          "lossless mapping", cur->meta[i - 1].name);
            op->result = -1;
            return;
        }
    }
    cur->row_ready = 1;
    op->result = 1;
    {
        int j;
        for (j = 0; j < cur->col_count; j++) {
            VmsValue* rv = &cur->row[j];
        }
        fflush(stderr);
    }
}

static VmsCursor* cursor_open_impl(VmsConnection* cn, const wchar_t* sql,
                                   const long long* params, int nparams,
                                   int shared, VmsError* err);
static void cursor_open_cleanup(VmsCursor* cur, int owns_row);

VmsCursor* vms_cursor_open_sql(VmsConnection* cn, const wchar_t* sql,
                               const long long* params, int nparams,
                               VmsError* err)
{
    return cursor_open_impl(cn, sql, params, nparams, 0, err);
}

/* R11 shared cursor: runs on the parent connection's own HDBC (MARS), so
 * its rows join the transaction identity. The parent must outlive the
 * cursor; the cursor never disconnects the shared HDBC. */
VmsCursor* vms_cursor_open_shared(VmsConnection* cn, const wchar_t* sql,
                                  const long long* params, int nparams,
                                  VmsError* err)
{
    return cursor_open_impl(cn, sql, params, nparams, 1, err);
}

/* shared-mode aware cleanup for failed cursor opens; owns_row frees the
 * row buffer that the caller already stored in cur */
static void cursor_open_cleanup(VmsCursor* cur, int owns_row)
{
    if (!cur) return;
    if (owns_row && cur->row) {
        int j;
        for (j = 0; j < cur->col_count; j++) value_clear(&cur->row[j]);
        free(cur->row);
        cur->row = NULL;
    }
    if (cur->hstmt) {
        SQLFreeStmt(cur->hstmt, SQL_CLOSE);
        SQLFreeHandle(SQL_HANDLE_STMT, cur->hstmt);
        cur->hstmt = NULL;
    }
    if (cur->own_hdbc) {
        if (cur->hdbc) {
            SQLDisconnect(cur->hdbc);
            SQLFreeHandle(SQL_HANDLE_DBC, cur->hdbc);
            cur->hdbc = NULL;
        }
        if (cur->worker) {
            vms_worker_stop(cur->worker);
            cur->worker = NULL;
        }
    }
    if (cur->meta) HeapFree(GetProcessHeap(), 0, cur->meta);
    if (cur->param_vals) HeapFree(GetProcessHeap(), 0, cur->param_vals);
    if (cur->param_inds) HeapFree(GetProcessHeap(), 0, cur->param_inds);
    HeapFree(GetProcessHeap(), 0, cur);
}

static VmsCursor* cursor_open_impl(VmsConnection* cn, const wchar_t* sql,
                                   const long long* params, int nparams,
                                   int shared, VmsError* err)
{
    VmsCursor* cur;
    OpCursorOpen open_op;
    OpCursorMeta meta_op;

    vms_error_ok(err);
    if (!cn || !sql) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "cursor open: bad args");
        return NULL;
    }

    cur = (VmsCursor*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(VmsCursor));
    if (!cur) {
        vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM cursor");
        return NULL;
    }
    cur->own_hdbc = shared ? 0 : 1;
    if (!shared) {
        /* independent lease: own HDBC from the connection's client env */
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, cn->client->env, &cur->hdbc))) {
            vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM cursor lease hdbc");
            HeapFree(GetProcessHeap(), 0, cur);
            return NULL;
        }
        cur->worker = vms_worker_start();
        if (!cur->worker) {
            vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM cursor worker");
            SQLFreeHandle(SQL_HANDLE_DBC, cur->hdbc);
            HeapFree(GetProcessHeap(), 0, cur);
            return NULL;
        }

        /* connect the lease from the parent's stored connstr */
        if (!cn->last_connstr) {
            vms_error_set(err, VMS_ERR_INTERNAL, NULL, 0,
                          "parent connection has no stored connstr for leases");
            vms_worker_stop(cur->worker);
            SQLFreeHandle(SQL_HANDLE_DBC, cur->hdbc);
            HeapFree(GetProcessHeap(), 0, cur);
            return NULL;
        }
        {
            SQLRETURN r = SQLDriverConnectW(cur->hdbc, NULL,
                                            (SQLWCHAR*)cn->last_connstr, SQL_NTS,
                                            NULL, 0, NULL, SQL_DRIVER_NOPROMPT);
            if (!(r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO)) {
                int q;
                diag_capture(err, SQL_HANDLE_DBC, cur->hdbc, "cursor lease connect", &q);
                vms_worker_stop(cur->worker);
                SQLFreeHandle(SQL_HANDLE_DBC, cur->hdbc);
                HeapFree(GetProcessHeap(), 0, cur);
                return NULL;
            }
        }
    } else {
        /* shared mode: reuse the parent HDBC and its worker (the parent
         * worker serializes calls on this connection) */
        cur->hdbc = cn->hdbc;
        cur->worker = cn->worker;
    }

    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, cur->hdbc, &cur->hstmt))) {
        vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM cursor stmt");
        if (shared) {
            HeapFree(GetProcessHeap(), 0, cur);
        } else {
            SQLDisconnect(cur->hdbc);
            SQLFreeHandle(SQL_HANDLE_DBC, cur->hdbc);
            vms_worker_stop(cur->worker);
            HeapFree(GetProcessHeap(), 0, cur);
        }
        return NULL;
    }

    /* bind integer parameters before execution; every return is checked */
    if (nparams > 0) {
        long long* vals;
        SQLLEN* inds;
        int k;
        int bind_ok = 1;
        vals = (long long*)HeapAlloc(GetProcessHeap(), 0, sizeof(long long) * (size_t)nparams);
        inds = (SQLLEN*)HeapAlloc(GetProcessHeap(), 0, sizeof(SQLLEN) * (size_t)nparams);
        if (!vals || !inds) {
            if (vals) HeapFree(GetProcessHeap(), 0, vals);
            if (inds) HeapFree(GetProcessHeap(), 0, inds);
            vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM param storage");
            cursor_open_cleanup(cur, 0);
            return NULL;
        }
        /* copy: the values must outlive deferred binding; ownership moves
         * to the cursor and is released at close */
        for (k = 0; k < nparams; k++) {
            SQLRETURN br;
            vals[k] = params[k];
            inds[k] = 0;
            br = SQLBindParameter(cur->hstmt, (SQLUSMALLINT)(k + 1), SQL_PARAM_INPUT,
                                  SQL_C_SBIGINT, SQL_BIGINT, 0, 0,
                                  (SQLPOINTER)(SIZE_T)&vals[k], 0, &inds[k]);
            if (!SQL_SUCCEEDED(br)) {
                int q;
                diag_capture(err, SQL_HANDLE_STMT, cur->hstmt, "SQLBindParameter", &q);
                bind_ok = 0;
                break;
            }
        }
        if (!bind_ok) {
            HeapFree(GetProcessHeap(), 0, vals);
            HeapFree(GetProcessHeap(), 0, inds);
            cursor_open_cleanup(cur, 0);
            return NULL;
        }
        cur->param_vals = vals;
        cur->param_inds = inds;
        cur->nparams = nparams;
    }

    open_op.cur = cur;
    open_op.sql = sql;
    open_op.err = err;
    open_op.ok = 0;
    vms_worker_run(cur->worker, job_cursor_open, &open_op);
    if (!open_op.ok) {
        if (cur->param_vals) HeapFree(GetProcessHeap(), 0, cur->param_vals);
        if (cur->param_inds) HeapFree(GetProcessHeap(), 0, cur->param_inds);
        cursor_open_cleanup(cur, 1);
        return NULL;
    }

    meta_op.cur = cur;
    meta_op.err = err;
    meta_op.ok = 0;
    vms_worker_run(cur->worker, job_cursor_meta, &meta_op);
    if (!meta_op.ok) {
        cursor_open_cleanup(cur, 0);
        return NULL;
    }
    return cur;
}

void vms_cursor_close(VmsCursor* cur)
{
    if (!cur) return;
    InterlockedExchange(&cur->closed, 1);
    if (cur->hstmt) {
        SQLFreeStmt(cur->hstmt, SQL_CLOSE);
        SQLFreeHandle(SQL_HANDLE_STMT, cur->hstmt);
        cur->hstmt = NULL;
    }
    if (cur->row) {
        int j;
        for (j = 0; j < cur->col_count; j++) value_clear(&cur->row[j]);
        free(cur->row);
        cur->row = NULL;
    }
    if (cur->param_vals) { HeapFree(GetProcessHeap(), 0, cur->param_vals); cur->param_vals = NULL; }
    if (cur->param_inds) { HeapFree(GetProcessHeap(), 0, cur->param_inds); cur->param_inds = NULL; }
    if (cur->meta) HeapFree(GetProcessHeap(), 0, cur->meta);
    if (cur->own_hdbc) {
        if (cur->hdbc) {
            SQLDisconnect(cur->hdbc);
            SQLFreeHandle(SQL_HANDLE_DBC, cur->hdbc);
        }
        if (cur->worker) vms_worker_stop(cur->worker);
    }
    /* shared mode: hdbc/worker belong to the parent — do not touch */
    HeapFree(GetProcessHeap(), 0, cur);
}

/* R6 compatibility wrapper: unparameterized full scan */
VmsCursor* vms_cursor_open(VmsConnection* cn, const char* schema,
                           const char* table, const VmsMetaColumn* cols,
                           int ncols, VmsError* err)
{
    wchar_t sql[2048];
    size_t used = 0;
    int i;

    vms_error_ok(err);
    if (!cn || !schema || !table || !cols || ncols <= 0) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "cursor open: bad args");
        return NULL;
    }
    if (!vms_meta_ident_valid(schema, 128) || !vms_meta_ident_valid(table, 128)) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "invalid identifier");
        return NULL;
    }
    {
        wchar_t head[64];
        wcscpy_s(sql, 2048, L"SELECT ");
        used = wcslen(sql);
        for (i = 0; i < ncols; i++) {
            wchar_t colw[256];
            int n;
            int ansi_text = (cols[i].vtype == VMS_CT_TEXT ||
                             cols[i].vtype == VMS_CT_BIGTEXT) &&
                            (!_stricmp(cols[i].type_name, "char") ||
                             !_stricmp(cols[i].type_name, "varchar") ||
                             !_stricmp(cols[i].type_name, "text") ||
                             !_stricmp(cols[i].type_name, "xml"));
            if (!vms_meta_ident_valid(cols[i].name, 128)) {
                vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0,
                              "invalid column name '%s'", cols[i].name);
                return NULL;
            }
            if (ansi_text) {
                if (used + 6 >= 2048) return NULL;
                wcscat_s(sql + used, 2048 - used, L"CAST(");
                used += 5;
            }
            n = MultiByteToWideChar(CP_UTF8, 0, cols[i].name, -1, colw + 1, 254);
            if (n <= 0 || used + (size_t)n + 4 >= 2048) {
                vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "projection too wide");
                return NULL;
            }
            colw[0] = L'[';
            colw[n] = L']';
            colw[n + 1] = 0;
            if (i) { sql[used++] = L','; sql[used] = 0; }
            wcscat_s(sql + used, 2048 - used, colw);
            used += (size_t)n + 1;
            if (ansi_text) {
                /* R12: ANSI text types stream as ANSI bytes through
                 * SQLGetData(SQL_C_BINARY); the nvarchar cast makes the
                 * value arrive as UTF-16 regardless of collation */
                wchar_t cast[32];
                int cn2 = MultiByteToWideChar(CP_UTF8, 0,
                        cols[i].vtype == VMS_CT_BIGTEXT ? " AS nvarchar(max))" : " AS nvarchar(4000))",
                        -1, cast, 32);
                if (cn2 <= 0 || used + (size_t)cn2 >= 2048) return NULL;
                wcscat_s(sql + used, 2048 - used, cast);
                used += (size_t)cn2 - 1;
            }
            if (cols[i].vtype == VMS_CT_SPATIAL) {
                /* R12: spatial columns project through STAsBinary() (the
                 * default WKB representation) */
                wchar_t meth[32];
                int m = MultiByteToWideChar(CP_UTF8, 0, ".STAsBinary()", -1,
                                            meth, 32);
                if (m > 0 && used + (size_t)m < 2048) {
                    wcscat_s(sql + used, 2048 - used, meth);
                    used += (size_t)m - 1;
                }
            }
        }
        _snwprintf_s(head, 64, _TRUNCATE, L"");
        (void)head;
    }
    _snwprintf_s(sql + used, 2048 - used, _TRUNCATE,
                 L" FROM [%hs].[%hs]", schema, table);
    return vms_cursor_open_sql(cn, sql, NULL, 0, err);
}

int vms_cursor_fetch(VmsCursor* cur, VmsError* err)
{
    OpCursorFetch op;
    if (!cur) return -1;
    op.cur = cur;
    op.err = err;
    op.result = -1;
    vms_worker_run(cur->worker, job_cursor_fetch, &op);
    return op.result;
}

int vms_cursor_cancel(VmsCursor* cur)
{
    if (!cur) return 0;
    vms_worker_cancel_active(cur->worker);
    return 1;
}

int vms_cursor_col_count(const VmsCursor* cur)
{
    return cur ? cur->col_count : 0;
}

const VmsColumnMeta* vms_cursor_meta(const VmsCursor* cur, int col)
{
    if (!cur || col < 0 || col >= cur->col_count) return NULL;
    return &cur->meta[col];
}

const VmsValue* vms_cursor_value(const VmsCursor* cur, int col)
{
    if (!cur || !cur->row_ready || col < 0 || col >= cur->col_count) return NULL;
    return &cur->row[col];
}




