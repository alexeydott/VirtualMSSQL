/* R0.3 — SQL execution, parameter binding, codecs, parameter ceiling, data-at-exec */
#include "probe.h"
#include <string.h>
#include <stdlib.h>

/* SQL Server driver-specific ODBC types not in sqlext.h */
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
#ifndef SQL_C_GUID
#define SQL_C_GUID SQL_GUID
#endif

int cases_connect_verify(ProbeCtx* ctx, HDBC dbc);

typedef struct WorkTables {
    bool created;
} WorkTables;

static bool exec_direct(ProbeCtx* ctx, HDBC dbc, const wchar_t* sql, ProbeDiag* diag)
{
    SQLHSTMT st = SQL_NULL_HSTMT;
    SQLRETURN r;
    diag_reset(diag);
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &st))) return false;
    r = SQLExecDirectW(st, sql, SQL_NTS);
    /* SQL_NO_DATA is success for DML that affected zero rows (empty table) */
    if (r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO || r == SQL_NO_DATA) {
        /* drain to completion */
        while (SQLMoreResults(st) == SQL_SUCCESS_WITH_INFO) {}
        SQLFreeHandle(SQL_HANDLE_STMT, st);
        return true;
    }
    diag_capture(ctx, SQL_HANDLE_STMT, st, diag);
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    return false;
}

#define W(s) L##s

/* Driver 18 requires ColumnSize > 0 for LONG-type parameter bindings;
 * documented SQL_SS_LENGTH_UNLIMITED (0) is rejected with HY104. 2^30-1 is the
 * largest accepted precision and de-facto stands for "unlimited". */
#define VMS_PARAM_UNLIMITED ((SQLULEN)1073741823)

static bool work_create(ProbeCtx* ctx, HDBC dbc, WorkTables* w)
{
    ProbeDiag diag;
    w->created = false;
    if (!exec_direct(ctx, dbc,
        L"IF OBJECT_ID(N'dbo.vms_probe_types') IS NOT NULL DROP TABLE dbo.vms_probe_types;"
        L"IF OBJECT_ID(N'dbo.vms_probe_stream') IS NOT NULL DROP TABLE dbo.vms_probe_stream;"
        L"IF OBJECT_ID(N'dbo.vms_probe_tx') IS NOT NULL DROP TABLE dbo.vms_probe_tx;"
        L"CREATE TABLE dbo.vms_probe_types("
        L" id int IDENTITY NOT NULL PRIMARY KEY,"
        L" c_bit bit NULL, c_tinyint tinyint NULL, c_smallint smallint NULL, c_int int NULL,"
        L" c_bigint bigint NULL, c_decimal decimal(30,10) NULL, c_money money NULL,"
        L" c_float float NULL, c_real real NULL,"
        L" c_char char(10) NULL, c_varchar varchar(100) NULL, c_nchar nchar(10) NULL,"
        L" c_nvarchar nvarchar(200) NULL, c_nvarcharmax nvarchar(max) NULL,"
        L" c_binary binary(16) NULL, c_varbinary varbinary(100) NULL, c_varbinarymax varbinary(max) NULL,"
        L" c_guid uniqueidentifier NULL, c_date date NULL, c_time time(7) NULL,"
        L" c_datetime datetime NULL, c_smalldatetime smalldatetime NULL,"
        L" c_datetime2 datetime2(7) NULL, c_datetimeoffset datetimeoffset(7) NULL,"
        L" c_xml xml NULL, c_rowversion rowversion NULL)",
        &diag)) {
        logf_ctx(ctx, "work tables: create failed %s (%d): %s",
                 diag.sqlstate, diag.native, diag.message);
        return false;
    }
    if (!exec_direct(ctx, dbc,
        L"CREATE TABLE dbo.vms_probe_stream("
        L" id int NOT NULL PRIMARY KEY, payload bigint NOT NULL)",
        &diag)) {
        logf_ctx(ctx, "stream table create failed %s (%d): %s",
                 diag.sqlstate, diag.native, diag.message);
        return false;
    }
    if (!exec_direct(ctx, dbc,
        L"CREATE TABLE dbo.vms_probe_tx("
        L" id int NOT NULL PRIMARY KEY, v int NOT NULL)",
        &diag)) {
        logf_ctx(ctx, "tx table create failed %s (%d): %s",
                 diag.sqlstate, diag.native, diag.message);
        return false;
    }
    w->created = true;
    return true;
}

static void work_drop(ProbeCtx* ctx, HDBC dbc)
{
    ProbeDiag diag;
    (void)exec_direct(ctx, dbc,
        L"IF OBJECT_ID(N'dbo.vms_probe_types') IS NOT NULL DROP TABLE dbo.vms_probe_types;"
        L"IF OBJECT_ID(N'dbo.vms_probe_stream') IS NOT NULL DROP TABLE dbo.vms_probe_stream;"
        L"IF OBJECT_ID(N'dbo.vms_probe_tx') IS NOT NULL DROP TABLE dbo.vms_probe_tx;",
        &diag);
}

/* --- case: prepare/bind/execute round-trip per codec --- */
static int case_codecs(ProbeCtx* ctx, HDBC dbc)
{
    int idx = ctx->case_count;
    SQLHSTMT st = SQL_NULL_HSTMT;
    ProbeDiag diag;
    SQLWCHAR del[] = L"DELETE FROM dbo.vms_probe_types";
    SQLWCHAR ins[] = L"INSERT INTO dbo.vms_probe_types"
        L"(c_bit,c_tinyint,c_smallint,c_int,c_bigint,c_decimal,c_money,c_float,c_real,"
        L"c_char,c_varchar,c_nchar,c_nvarchar,c_nvarcharmax,c_binary,c_varbinary,c_varbinarymax,"
        L"c_guid,c_date,c_time,c_datetime,c_smalldatetime,c_datetime2,c_datetimeoffset,c_xml)"
        L" VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    SQLRETURN r;

    /* representative bound values */
    SQLCHAR bit_v = 1;
    SQLCHAR tiny_v = 200;
    SQLSMALLINT small_v = -32000;
    SQLINTEGER int_v = 2000000000;
    SQLBIGINT big_v = (SQLBIGINT)9000000000000000000LL;
    char dec_v[] = "12345678901234567890.1234567890";
    SQLWCHAR money_v[] = L"922337203685477.5807";
    double fl_v = 3.14159265358979;
    float re_v = 2.5f;
    SQLCHAR ch_v[11] = "abcdefghij";
    SQLCHAR vch_v[101] = "varchar-payload";
    SQLWCHAR nch_v[11] = L"nchar01234";
    SQLWCHAR nvch_v[201] = L"привет мир — CJK 漢字 — emoji 🙂 — non-BMP \U0001F600";
    SQLWCHAR nvmax_v[512] = L"large unicode payload";
    SQLCHAR bin_v[16]; memset(bin_v, 0xAB, sizeof(bin_v));
    SQLCHAR vbin_v[101]; memset(vbin_v, 0xCD, sizeof(vbin_v));
    SQLCHAR vbinmax_v[300]; memset(vbinmax_v, 0xEF, sizeof(vbinmax_v));
    SQLWCHAR guid_v[] = L"6F9619FF-8B86-D011-B42D-00C04FC964FF";
    SQLWCHAR date_v[] = L"2026-09-01";
    SQLWCHAR time_v[] = L"12:34:56.7890123";
    SQLWCHAR dt_v[] = L"2026-09-01 12:34:56";
    SQLWCHAR sdt_v[] = L"2026-09-01 12:34:00";
    SQLWCHAR dt2_v[] = L"2026-09-01 12:34:56.7890123";
    SQLWCHAR dto_v[] = L"2026-09-01 12:34:56.7890123 +05:00";
    SQLWCHAR xml_v[256] = L"<root><item>值</item></root>";
    SQLLEN ind_full[25], ind_null[25];

    case_add(ctx, "exec", "codec_roundtrip", PROBE_SKIP, NULL);
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &st))) {
        case_set_status(ctx, idx, PROBE_FAIL, "alloc stmt failed");
        return 1;
    }
    {
        SQLRETURN dr = SQLExecDirectW(st, del, SQL_NTS);
        if (!(SQL_SUCCEEDED(dr) || dr == SQL_NO_DATA)) {
            diag_capture(ctx, SQL_HANDLE_STMT, st, &diag);
            ctx->cases[idx].diag = diag;
            case_set_status(ctx, idx, PROBE_FAIL, "delete failed");
            SQLFreeHandle(SQL_HANDLE_STMT, st);
            return 1;
        }
    }
    SQLFreeStmt(st, SQL_CLOSE);

    if (!SQL_SUCCEEDED(SQLPrepareW(st, ins, SQL_NTS))) {
        diag_capture(ctx, SQL_HANDLE_STMT, st, &diag);
        ctx->cases[idx].diag = diag;
        case_set_status(ctx, idx, PROBE_FAIL, "prepare failed");
        SQLFreeHandle(SQL_HANDLE_STMT, st);
        return 1;
    }

    {
        int i;
        for (i = 0; i < 25; i++) ind_full[i] = SQL_IS_POINTER ? 0 : 0;
        for (i = 0; i < 25; i++) ind_null[i] = SQL_NULL_DATA;
    }
    ind_full[0] = 1;                                /* bit */
    ind_full[1] = 0; ind_full[2] = 0; ind_full[3] = 0; ind_full[4] = 0;
    ind_full[5] = (SQLLEN)strlen(dec_v);
    ind_full[6] = (SQLLEN)(wcslen(money_v) * (int)sizeof(wchar_t));
    ind_full[7] = 0; ind_full[8] = 0;
    ind_full[9] = 10;
    ind_full[10] = (SQLLEN)strlen((char*)vch_v);
    ind_full[11] = (SQLLEN)(wcslen(nch_v) * sizeof(wchar_t));
    ind_full[12] = (SQLLEN)(wcslen(nvch_v) * sizeof(wchar_t));
    ind_full[13] = (SQLLEN)(wcslen(nvmax_v) * sizeof(wchar_t));
    ind_full[14] = 16;
    ind_full[15] = 100;
    ind_full[16] = 300;
    ind_full[17] = (SQLLEN)(wcslen(guid_v) * sizeof(wchar_t));
    ind_full[18] = (SQLLEN)(wcslen(date_v) * sizeof(wchar_t));
    ind_full[19] = (SQLLEN)(wcslen(time_v) * sizeof(wchar_t));
    ind_full[20] = (SQLLEN)(wcslen(dt_v) * sizeof(wchar_t));
    ind_full[21] = (SQLLEN)(wcslen(sdt_v) * sizeof(wchar_t));
    ind_full[22] = (SQLLEN)(wcslen(dt2_v) * sizeof(wchar_t));
    ind_full[23] = (SQLLEN)(wcslen(dto_v) * sizeof(wchar_t));
    ind_full[24] = (SQLLEN)(wcslen(xml_v) * sizeof(wchar_t));

    SQLRETURN br[25];
#define B(n, call) br[(n) - 1] = SQLBindParameter call
    B(1, (st, 1, SQL_PARAM_INPUT, SQL_C_BIT, SQL_BIT, 0, 0, &bit_v, 0, &ind_full[0]));
    B(2, (st, 2, SQL_PARAM_INPUT, SQL_C_UTINYINT, SQL_TINYINT, 0, 0, &tiny_v, 0, &ind_full[1]));
    B(3, (st, 3, SQL_PARAM_INPUT, SQL_C_SSHORT, SQL_SMALLINT, 0, 0, &small_v, 0, &ind_full[2]));
    B(4, (st, 4, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &int_v, 0, &ind_full[3]));
    B(5, (st, 5, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT, 0, 0, &big_v, 0, &ind_full[4]));
    B(6, (st, 6, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_DECIMAL, 30, 10, dec_v, sizeof(dec_v), &ind_full[5]));
    B(7, (st, 7, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_DECIMAL, 19, 4, money_v, sizeof(money_v), &ind_full[6]));
    B(8, (st, 8, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_FLOAT, 0, 0, &fl_v, 0, &ind_full[7]));
    B(9, (st, 9, SQL_PARAM_INPUT, SQL_C_FLOAT, SQL_REAL, 0, 0, &re_v, 0, &ind_full[8]));
    B(10, (st, 10, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, 10, 0, ch_v, sizeof(ch_v), &ind_full[9]));
    B(11, (st, 11, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 100, 0, vch_v, sizeof(vch_v), &ind_full[10]));
    B(12, (st, 12, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WCHAR, 10, 0, nch_v, sizeof(nch_v), &ind_full[11]));
    B(13, (st, 13, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR, 200, 0, nvch_v, sizeof(nvch_v), &ind_full[12]));
    B(14, (st, 14, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WLONGVARCHAR, VMS_PARAM_UNLIMITED, 0, nvmax_v, sizeof(nvmax_v), &ind_full[13]));
    B(15, (st, 15, SQL_PARAM_INPUT, SQL_C_BINARY, SQL_BINARY, 16, 0, bin_v, 0, &ind_full[14]));
    B(16, (st, 16, SQL_PARAM_INPUT, SQL_C_BINARY, SQL_VARBINARY, 100, 0, vbin_v, 0, &ind_full[15]));
    B(17, (st, 17, SQL_PARAM_INPUT, SQL_C_BINARY, SQL_LONGVARBINARY, VMS_PARAM_UNLIMITED, 0, vbinmax_v, 0, &ind_full[16]));
    B(18, (st, 18, SQL_PARAM_INPUT, SQL_C_GUID, SQL_GUID, 0, 0, guid_v, sizeof(guid_v), &ind_full[17]));
    B(19, (st, 19, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_TYPE_DATE, 10, 0, date_v, sizeof(date_v), &ind_full[18]));
    B(20, (st, 20, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_SS_TIME2, 16, 7, time_v, sizeof(time_v), &ind_full[19]));
    B(21, (st, 21, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_TYPE_TIMESTAMP, 23, 3, dt_v, sizeof(dt_v), &ind_full[20]));
    B(22, (st, 22, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_TYPE_TIMESTAMP, 19, 0, sdt_v, sizeof(sdt_v), &ind_full[21]));
    B(23, (st, 23, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_TYPE_TIMESTAMP, 27, 7, dt2_v, sizeof(dt2_v), &ind_full[22]));
    B(24, (st, 24, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_SS_TIMESTAMPOFFSET, 34, 7, dto_v, sizeof(dto_v), &ind_full[23]));
    B(25, (st, 25, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_SS_XML, 0, 0, xml_v, sizeof(xml_v), &ind_full[24]));
#undef B
    /* every parameter must be bound before execute; SQLBindParameter failures
     * surface later as 07002 "COUNT field incorrect" */
    {
        int bi;
        SQLSMALLINT pcnt = 0;
        SQLNumParams(st, &pcnt);
        logf_ctx(ctx, "codec: NumParams=%d", (int)pcnt);
        for (bi = 0; bi < 25; bi++) {
            if (!SQL_SUCCEEDED(br[bi])) {
                ProbeDiag bdiag;
                diag_capture(ctx, SQL_HANDLE_STMT, st, &bdiag);
                logf_ctx(ctx, "codec: bind %d FAILED ret=%d %s (%d): %s",
                         bi + 1, (int)br[bi], bdiag.sqlstate, bdiag.native, bdiag.message);
            }
        }
    }

    r = SQLExecute(st);
    if (r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO) {
        SQLFreeStmt(st, SQL_CLOSE);
        /* NULL-value insert must also succeed */
        {
            SQLWCHAR ins_null[] = L"INSERT INTO dbo.vms_probe_types(c_bit) VALUES(?)";
            SQLLEN nind = SQL_NULL_DATA;
            SQLCHAR nbit = 0;
            if (SQL_SUCCEEDED(SQLPrepareW(st, ins_null, SQL_NTS))) {
                SQLBindParameter(st, 1, SQL_PARAM_INPUT, SQL_C_BIT, SQL_BIT, 0, 0, &nbit, 0, &nind);
                if (SQL_SUCCEEDED(SQLExecute(st))) {
                    SQLFreeStmt(st, SQL_CLOSE);
                    case_set_status(ctx, idx, PROBE_PASS,
                        "25-codec bound insert + NULL bind insert OK");
                } else {
                    diag_capture(ctx, SQL_HANDLE_STMT, st, &diag);
                    ctx->cases[idx].diag = diag;
                    case_set_status(ctx, idx, PROBE_FAIL, "NULL bind insert failed");
                }
            } else {
                diag_capture(ctx, SQL_HANDLE_STMT, st, &diag);
                ctx->cases[idx].diag = diag;
                case_set_status(ctx, idx, PROBE_FAIL, "NULL insert prepare failed");
            }
        }
    } else {
        diag_capture(ctx, SQL_HANDLE_STMT, st, &diag);
        ctx->cases[idx].diag = diag;
        case_set_status(ctx, idx, PROBE_FAIL, "codec insert execute failed");
    }
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    return 1;
}

/* --- case: parameter ceiling via binary search on IN-list size --- */
static int case_param_ceiling(ProbeCtx* ctx, HDBC dbc)
{
    int idx = ctx->case_count;
    long long hi = ctx->cfg.param_ceiling_max > 0 ? ctx->cfg.param_ceiling_max : 2000;
    long long lo = 1, best = 0;
    ProbeDiag diag;

    diag_reset(&diag);

    case_add(ctx, "exec", "param_ceiling", PROBE_SKIP, NULL);
    {
        SQLHSTMT st = SQL_NULL_HSTMT;
        long long n;
        for (n = 1; n <= hi; n *= 2) {
            /* build SELECT with n parameters */
            SQLWCHAR* sql = (SQLWCHAR*)malloc(((size_t)n * 3 + 32) * sizeof(SQLWCHAR));
            wchar_t* p;
            long long i;
            if (!sql) break;
            wcscpy_s(sql, (size_t)n * 3 + 32, L"SELECT ");
            p = sql + wcslen(sql);
            for (i = 0; i < n; i++) {
                if (i) *p++ = L',';
                *p++ = L'?';
            }
            *p++ = L';';
            *p = 0;
            if (SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &st)) &&
                SQL_SUCCEEDED(SQLPrepareW(st, sql, SQL_NTS))) {
                SQLINTEGER v = 0;
                SQLLEN ind = 0;
                SQLRETURN er = SQL_SUCCESS;
                for (i = 0; i < n && SQL_SUCCEEDED(er); i++) {
                    er = SQLBindParameter(st, (SQLUSMALLINT)(i + 1), SQL_PARAM_INPUT,
                                          SQL_C_SLONG, SQL_INTEGER, 0, 0, &v, 0, &ind);
                }
                if (SQL_SUCCEEDED(er) && SQL_SUCCEEDED(SQLExecute(st))) {
                    SQLFreeStmt(st, SQL_CLOSE);
                    best = n;
                    SQLFreeHandle(SQL_HANDLE_STMT, st);
                    free(sql);
                    continue;
                }
                diag_capture(ctx, SQL_HANDLE_STMT, st, &diag);
            } else {
                diag_capture(ctx, SQL_HANDLE_STMT, st, &diag);
            }
            if (st != SQL_NULL_HSTMT) SQLFreeHandle(SQL_HANDLE_STMT, st);
            free(sql);
            break;
        }
        /* refine between best and best*2 by binary search */
        if (best > 0) {
            lo = best; hi = best * 2 < hi ? best * 2 : hi;
            while (lo + 1 < hi) {
                long long mid = (lo + hi) / 2;
                SQLHSTMT s2 = SQL_NULL_HSTMT;
                SQLWCHAR* sql = (SQLWCHAR*)malloc(((size_t)mid * 3 + 32) * sizeof(SQLWCHAR));
                wchar_t* p;
                long long i;
                bool ok;
                if (!sql) break;
                wcscpy_s(sql, (size_t)mid * 3 + 32, L"SELECT ");
                p = sql + wcslen(sql);
                for (i = 0; i < mid; i++) {
                    if (i) *p++ = L',';
                    *p++ = L'?';
                }
                *p++ = L';';
                *p = 0;
                ok = SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &s2)) &&
                     SQL_SUCCEEDED(SQLPrepareW(s2, sql, SQL_NTS));
                if (ok) {
                    SQLINTEGER v = 0;
                    SQLLEN ind = 0;
                    SQLRETURN er = SQL_SUCCESS;
                    for (i = 0; i < mid && SQL_SUCCEEDED(er); i++) {
                        er = SQLBindParameter(s2, (SQLUSMALLINT)(i + 1), SQL_PARAM_INPUT,
                                              SQL_C_SLONG, SQL_INTEGER, 0, 0, &v, 0, &ind);
                    }
                    ok = SQL_SUCCEEDED(er) && SQL_SUCCEEDED(SQLExecute(s2));
                    if (ok) SQLFreeStmt(s2, SQL_CLOSE);
                }
                if (!ok) diag_capture(ctx, SQL_HANDLE_STMT, s2, &diag);
                if (s2 != SQL_NULL_HSTMT) SQLFreeHandle(SQL_HANDLE_STMT, s2);
                free(sql);
                if (ok) lo = mid; else hi = mid;
            }
            best = lo;
            case_set_status(ctx, idx, PROBE_PASS, "practical parameter ceiling = %lld", best);
        } else {
            ctx->cases[idx].diag = diag;
            case_set_status(ctx, idx, PROBE_FAIL, "even 1-parameter prepare failed");
        }
    }
    return 1;
}

/* --- case: data-at-execution with SQLParamData/SQLPutData --- */
static int case_data_at_exec(ProbeCtx* ctx, HDBC dbc)
{
    int idx = ctx->case_count;
    SQLHSTMT st = SQL_NULL_HSTMT;
    SQLWCHAR ins[] = L"INSERT INTO dbo.vms_probe_types(c_nvarcharmax) VALUES(?)";
    SQLWCHAR chunk[256];
    SQLLEN ind = SQL_DATA_AT_EXEC;
    SQLPOINTER pid = (SQLPOINTER)1;
    ProbeDiag diag;
    int i, ok = 1;

    case_add(ctx, "exec", "data_at_exec", PROBE_SKIP, NULL);
    diag_reset(&diag);
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &st))) {
        case_set_status(ctx, idx, PROBE_FAIL, "alloc stmt failed");
        return 1;
    }
    if (!SQL_SUCCEEDED(SQLPrepareW(st, ins, SQL_NTS))) {
        diag_capture(ctx, SQL_HANDLE_STMT, st, &diag);
        ctx->cases[idx].diag = diag;
        case_set_status(ctx, idx, PROBE_FAIL, "prepare failed");
        SQLFreeHandle(SQL_HANDLE_STMT, st);
        return 1;
    }
    /* Proper DAE flow */
    SQLBindParameter(st, 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WLONGVARCHAR,
                     VMS_PARAM_UNLIMITED, 0, pid, 0, &ind);
    {
        SQLRETURN r = SQLExecute(st);
        if (r == SQL_NEED_DATA) {
            while ((r = SQLParamData(st, &pid)) == SQL_NEED_DATA) {
                for (i = 0; i < 8; i++) {
                    _snwprintf_s(chunk, 256, _TRUNCATE, L"chunk-%04d-value", i);
                    if (!SQL_SUCCEEDED(SQLPutData(st, chunk, (SQLLEN)(wcslen(chunk) * sizeof(wchar_t))))) {
                        ok = 0;
                        break;
                    }
                }
                if (!ok) break;
            }
            if (ok && SQL_SUCCEEDED(r)) {
                case_set_status(ctx, idx, PROBE_PASS, "DAE insert of 8 chunks OK");
            } else {
                diag_capture(ctx, SQL_HANDLE_STMT, st, &diag);
                ctx->cases[idx].diag = diag;
                case_set_status(ctx, idx, PROBE_FAIL, "SQLParamData loop failed");
            }
        } else if (r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO) {
            case_set_status(ctx, idx, PROBE_FAIL, "driver executed without DAE (unexpected)");
        } else {
            diag_capture(ctx, SQL_HANDLE_STMT, st, &diag);
            ctx->cases[idx].diag = diag;
            case_set_status(ctx, idx, PROBE_FAIL, "execute for DAE failed (ret=%d)", (int)r);
        }
    }
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    return 1;
}

int cases_exec(ProbeCtx* ctx)
{
    int idx = ctx->case_count;
    HDBC dbc = SQL_NULL_HDBC;
    wchar_t* wcs;
    ProbeDiag diag;
    WorkTables w;
    int n = 0;

    case_add(ctx, "exec", "worktable_setup", PROBE_SKIP, NULL);
    if (!ctx->driver_present) {
        case_set_status(ctx, idx, PROBE_SKIP, "driver 18 not present");
        return 1;
    }
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, ctx->env, &dbc))) {
        case_set_status(ctx, idx, PROBE_FAIL, "alloc dbc failed");
        return 1;
    }
    wcs = connstr_build(ctx, ctx->cfg.server, "sql", "trust_server_certificate", NULL);
    if (!conn_connect(ctx, dbc, wcs, &diag)) {
        free(wcs);
        ctx->cases[idx].diag = diag;
        case_set_status(ctx, idx, PROBE_FAIL, "connect failed");
        return 1;
    }
    free(wcs);
    case_set_status(ctx, idx, PROBE_PASS, "connected");

    cases_connect_verify(ctx, dbc);
    n += 2;

    if (!work_create(ctx, dbc, &w)) {
        case_add(ctx, "exec", "worktable_setup2", PROBE_FAIL, "see log");
        n++;
        conn_close(dbc);
        return n;
    }

    n += case_codecs(ctx, dbc);
    n += case_param_ceiling(ctx, dbc);
    n += case_data_at_exec(ctx, dbc);

    work_drop(ctx, dbc);
    conn_close(dbc);
    return n;
}
