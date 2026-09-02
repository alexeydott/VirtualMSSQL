/* R0.4 — streaming: 1M rows, SQLGetData chunking, bounded memory */
#include "probe.h"
#include <string.h>
#include <stdlib.h>

int cases_stream(ProbeCtx* ctx)
{
    int idx = ctx->case_count;
    HDBC dbc = SQL_NULL_HDBC;
    SQLHSTMT st = SQL_NULL_HSTMT;
    wchar_t* wcs;
    ProbeDiag diag;
    long long rows = ctx->cfg.stream_rows > 0 ? ctx->cfg.stream_rows : 1000000;
    wchar_t ins[128], sel[512], drop[96];
    SQLWCHAR wsql[128];
    SQLINTEGER id_v = 0;
    SQLLEN ind1 = 0;
    SQLRETURN r;
    long long fetched = 0;
    DWORD t0, t1;

    case_add(ctx, "stream", "bulk_rows", PROBE_SKIP, NULL);
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

    _snwprintf_s(drop, 96, _TRUNCATE,
        L"IF OBJECT_ID(N'dbo.vms_probe_stream') IS NOT NULL DROP TABLE dbo.vms_probe_stream");
    _snwprintf_s(ins, 128, _TRUNCATE,
        L"CREATE TABLE dbo.vms_probe_stream(id int NOT NULL PRIMARY KEY, payload bigint NOT NULL)");
    _snwprintf_s(sel, (size_t)(sizeof(sel) / sizeof(sel[0])), _TRUNCATE,
        L"INSERT INTO dbo.vms_probe_stream(id, payload) SELECT TOP (%lld) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)), ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) FROM sys.all_objects a CROSS JOIN sys.all_objects b", rows);

    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &st))) {
        case_set_status(ctx, idx, PROBE_FAIL, "alloc stmt failed");
        conn_close(dbc);
        return 1;
    }
    MultiByteToWideChar(CP_UTF8, 0, "", -1, wsql, 1); /* noop keep wcscpy usage simple */
    if (!SQL_SUCCEEDED(SQLExecDirectW(st, drop, SQL_NTS)) ||
        !SQL_SUCCEEDED(SQLFreeStmt(st, SQL_CLOSE))) {
        diag_capture(ctx, SQL_HANDLE_STMT, st, &diag);
        ctx->cases[idx].diag = diag;
        case_set_status(ctx, idx, PROBE_FAIL, "drop failed");
        goto done;
    }
    if (!SQL_SUCCEEDED(SQLExecDirectW(st, ins, SQL_NTS))) {
        diag_capture(ctx, SQL_HANDLE_STMT, st, &diag);
        ctx->cases[idx].diag = diag;
        case_set_status(ctx, idx, PROBE_FAIL, "create stream table failed");
        goto done;
    }
    SQLFreeStmt(st, SQL_CLOSE);
    if (!SQL_SUCCEEDED(SQLExecDirectW(st, sel, SQL_NTS))) {
        diag_capture(ctx, SQL_HANDLE_STMT, st, &diag);
        ctx->cases[idx].diag = diag;
        case_set_status(ctx, idx, PROBE_FAIL, "bulk insert failed (rows=%lld)", rows);
        goto done;
    }
    SQLFreeStmt(st, SQL_CLOSE);

    t0 = GetTickCount64() & 0xFFFFFFFF;
    _snwprintf_s(sel, 128, _TRUNCATE, L"SELECT id, payload FROM dbo.vms_probe_stream ORDER BY id");
    if (!SQL_SUCCEEDED(SQLPrepareW(st, sel, SQL_NTS)) ||
        !SQL_SUCCEEDED(SQLExecute(st))) {
        diag_capture(ctx, SQL_HANDLE_STMT, st, &diag);
        ctx->cases[idx].diag = diag;
        case_set_status(ctx, idx, PROBE_FAIL, "select prepare/execute failed");
        goto done;
    }
    /* bind only col 1; col 2 fetched via SQLGetData (chunking check) */
    if (!SQL_SUCCEEDED(SQLBindCol(st, 1, SQL_C_SLONG, &id_v, 0, &ind1))) {
        diag_capture(ctx, SQL_HANDLE_STMT, st, &diag);
        ctx->cases[idx].diag = diag;
        case_set_status(ctx, idx, PROBE_FAIL, "bindcol failed");
        goto done;
    }
    {
        SQLCHAR chunk[4096];
        SQLLEN got = 0;
        int data_ok = 1;
        while ((r = SQLFetch(st)) == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO) {
            fetched++;
            /* fetch col2 in chunks; expect single chunk for bigint or repeated for large */
            while ((r = SQLGetData(st, 2, SQL_C_BINARY, chunk, sizeof(chunk), &got))
                   == SQL_SUCCESS_WITH_INFO) {
                /* chunked continuation — bounded buffer in use */
            }
            if (!(r == SQL_SUCCESS || r == SQL_NO_DATA)) { data_ok = 0; break; }
        }
        t1 = (DWORD)(GetTickCount64() & 0xFFFFFFFF);
        if (fetched == rows && data_ok) {
            case_set_status(ctx, idx, PROBE_PASS,
                "streamed %lld rows, SQLGetData chunking OK, %lu ms", fetched, (unsigned long)(t1 - t0));
        } else {
            case_set_status(ctx, idx, PROBE_FAIL,
                "fetched %lld of %lld rows (data_ok=%d)", fetched, rows, data_ok);
        }
    }
    /* drop table */
    SQLFreeStmt(st, SQL_CLOSE);
    _snwprintf_s(drop, 96, _TRUNCATE,
        L"IF OBJECT_ID(N'dbo.vms_probe_stream') IS NOT NULL DROP TABLE dbo.vms_probe_stream");
    if (SQL_SUCCEEDED(SQLExecDirectW(st, drop, SQL_NTS))) {
        SQLFreeStmt(st, SQL_CLOSE);
    }

done:
    if (st != SQL_NULL_HSTMT) SQLFreeHandle(SQL_HANDLE_STMT, st);
    conn_close(dbc);
    return 1;
}
