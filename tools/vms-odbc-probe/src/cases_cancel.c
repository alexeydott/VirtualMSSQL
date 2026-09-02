/* R0.5 — cancellation: connection-affine worker, SQLCancelHandle, interrupt prototype */
#include "probe.h"
#include <string.h>
#include <stdlib.h>
#include <process.h>

typedef struct CancelJob {
    HDBC dbc;
    volatile LONG start;
    volatile LONG done;
    volatile LONG cancelled;
    volatile SQLHSTMT active_stmt;  /* published so the canceller can target the stmt */
    SQLRETURN exec_ret;
    ProbeDiag diag;
} CancelJob;

static unsigned __stdcall worker_exec(void* arg)
{
    CancelJob* j = (CancelJob*)arg;
    SQLHSTMT st = SQL_NULL_HSTMT;
    /* intentionally slow query: recursive CTE counting to 100 million */
    SQLWCHAR slow[] = L"WITH c AS (SELECT CAST(1 AS bigint) AS n "
                      L"UNION ALL SELECT n+1 FROM c WHERE n < 100000000) "
                      L"SELECT COUNT(*) FROM c OPTION (MAXRECURSION 0)";
    while (!InterlockedCompareExchange(&j->start, 0, 0)) Sleep(1);
    if (SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, j->dbc, &st))) {
        /* publish the active stmt: cross-thread cancel must target SQL_HANDLE_STMT
         * (SQLCancelHandle(SQL_HANDLE_DBC) only cancels connection-level ops) */
        InterlockedExchangePointer((volatile PVOID*)&j->active_stmt, (PVOID)st);
        j->exec_ret = SQLExecDirectW(st, slow, SQL_NTS);
        InterlockedExchangePointer((volatile PVOID*)&j->active_stmt, NULL);
        if (j->exec_ret == SQL_SUCCESS || j->exec_ret == SQL_SUCCESS_WITH_INFO) {
            SQLFreeStmt(st, SQL_CLOSE);
            SQLFreeHandle(SQL_HANDLE_STMT, st);
            InterlockedExchange(&j->done, 1);
            return 0;
        }
        diag_capture(NULL, SQL_HANDLE_STMT, st, &j->diag);
        SQLFreeHandle(SQL_HANDLE_STMT, st);
    }
    InterlockedExchange(&j->done, 1);
    return 0;
}

static SQLHSTMT job_active_stmt(CancelJob* j)
{
    return (SQLHSTMT)InterlockedCompareExchangePointer((volatile PVOID*)&j->active_stmt, NULL, NULL);
}

static int case_cancel_query(ProbeCtx* ctx, HDBC dbc)
{
    int idx = ctx->case_count;
    CancelJob j;
    HANDLE th;
    DWORD t0;
    int pass = 0;

    memset(&j, 0, sizeof(j));
    j.dbc = dbc;
    j.exec_ret = SQL_INVALID_HANDLE;

    case_add(ctx, "cancel", "cross_thread", PROBE_SKIP, NULL);
    th = (HANDLE)_beginthreadex(NULL, 0, worker_exec, &j, 0, NULL);
    if (!th) {
        case_set_status(ctx, idx, PROBE_FAIL, "cannot start worker thread");
        return 1;
    }
    InterlockedExchange(&j.start, 1);
    t0 = GetTickCount();
    /* let the query reach the server, then cancel the worker's active statement */
    Sleep(2000);
    {
        SQLHSTMT st = job_active_stmt(&j);
        if (st) {
            SQLRETURN cr = SQLCancelHandle(SQL_HANDLE_STMT, st);
            if (cr == SQL_SUCCESS || cr == SQL_SUCCESS_WITH_INFO) {
                InterlockedExchange(&j.cancelled, 1);
            }
        }
    }
    /* never leave the worker running past this function: its stack slot j dies here */
    WaitForSingleObject(th, 120000);
    CloseHandle(th);
    {
        DWORD elapsed = GetTickCount() - t0;
        if (j.done && j.cancelled && elapsed < 15000 &&
            !(j.exec_ret == SQL_SUCCESS || j.exec_ret == SQL_SUCCESS_WITH_INFO)) {
            pass = 1;
        }
        if (pass) {
            case_set_status(ctx, idx, PROBE_PASS,
                "long query cancelled via SQLCancelHandle(SQL_HANDLE_STMT) in %lu ms (ret != SUCCESS)",
                (unsigned long)elapsed);
            if (j.diag.present) {
                ctx->cases[idx].diag = j.diag;
            }
        } else {
            case_set_status(ctx, idx, PROBE_FAIL,
                "cancel ineffective (done=%d cancelled=%d elapsed=%lu ret=%d)",
                j.done, j.cancelled, (unsigned long)elapsed, (int)j.exec_ret);
            if (j.diag.present) {
                ctx->cases[idx].diag = j.diag;
            }
        }
    }
    return 1;
}

/* simulate sqlite3_interrupt prototype: external flag -> SQLCancelHandle; same mechanics
   as cross-thread cancel but from "interrupt hook" perspective */
static volatile LONG g_interrupt_flag = 0;

static int case_interrupt_prototype(ProbeCtx* ctx, HDBC dbc)
{
    int idx = ctx->case_count;
    CancelJob j;
    HANDLE th;
    int pass = 0;

    memset(&j, 0, sizeof(j));
    j.dbc = dbc;

    case_add(ctx, "cancel", "interrupt_prototype", PROBE_SKIP, NULL);
    th = (HANDLE)_beginthreadex(NULL, 0, worker_exec, &j, 0, NULL);
    if (!th) {
        case_set_status(ctx, idx, PROBE_FAIL, "cannot start worker thread");
        return 1;
    }
    InterlockedExchange(&j.start, 1);
    Sleep(1500);
    /* prototype: interrupt hook sets flag; poller thread translates to SQLCancelHandle
     * on the worker's published statement handle */
    InterlockedExchange(&g_interrupt_flag, 1);
    if (InterlockedCompareExchange(&g_interrupt_flag, 0, 0)) {
        SQLHSTMT st;
        InterlockedExchange(&g_interrupt_flag, 0);
        st = job_active_stmt(&j);
        if (st) SQLCancelHandle(SQL_HANDLE_STMT, st);
        }
    WaitForSingleObject(th, 120000);
    CloseHandle(th);
    if (j.done && !(j.exec_ret == SQL_SUCCESS || j.exec_ret == SQL_SUCCESS_WITH_INFO)) {
        pass = 1;
    }
    if (pass) {
        case_set_status(ctx, idx, PROBE_PASS,
            "interrupt-flag -> SQLCancelHandle prototype stops remote work; handles reusable");
        /* verify connection still usable after cancel */
        {
            SQLHSTMT st = SQL_NULL_HSTMT;
            SQLWCHAR q[] = L"SELECT 1";
            ProbeDiag diag2;
            if (SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &st)) &&
                SQL_SUCCEEDED(SQLExecDirectW(st, q, SQL_NTS))) {
                SQLFreeStmt(st, SQL_CLOSE);
                SQLFreeHandle(SQL_HANDLE_STMT, st);
                logf_ctx(ctx, "cancel: connection reusable after cancel");
            } else {
                diag_capture(ctx, SQL_HANDLE_STMT, st, &diag2);
                ctx->cases[idx].diag = diag2;
                case_set_status(ctx, idx, PROBE_FAIL, "connection unusable after cancel");
            }
        }
    } else {
        case_set_status(ctx, idx, PROBE_FAIL, "interrupt prototype failed to stop query");
    }
    return 1;
}

int cases_cancel(ProbeCtx* ctx)
{
    int idx = ctx->case_count;
    HDBC dbc = SQL_NULL_HDBC;
    wchar_t* wcs;
    ProbeDiag diag;

    case_add(ctx, "cancel", "setup", PROBE_SKIP, NULL);
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

    case_cancel_query(ctx, dbc);
    case_interrupt_prototype(ctx, dbc);

    conn_close(dbc);
    return 3;
}
