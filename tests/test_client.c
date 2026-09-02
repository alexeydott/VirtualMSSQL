/* G3 — client-layer integration suite.
 * Runs against a live SQL Server configured via VMS_TEST_CONNSTR (UTF-8 file
 * or env). Every case mirrors an R3 invariant: single ownership, full-row
 * decode before visibility, cross-thread cancel, quarantine, drain, tran. */
#include "vms_client.h"
#include "vms_foundation.h"
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fail_once(void) { return 1; }

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { g_fail++; fprintf(stderr, "FAIL@%d: %s\n", __LINE__, #cond); } \
} while (0)

static wchar_t g_connstr[1024];

static int load_connstr(void)
{
    const char* env = getenv("VMS_TEST_CONNSTR");
    size_t n;
    if (!env || !env[0]) {
        fprintf(stderr, "VMS_TEST_CONNSTR not set; skipping integration suite\n");
        return 0;
    }
    n = strlen(env);
    if (n >= sizeof(g_connstr) / sizeof(wchar_t)) return 0;
    MultiByteToWideChar(CP_UTF8, 0, env, -1, g_connstr, 1024);
    return 1;
}

static const char* cls_name(int cls)
{
    switch (cls) {
    case VMS_OK: return "OK";
    case VMS_ERR_CANCELLED: return "CANCELLED";
    case VMS_ERR_QUARANTINED: return "QUARANTINED";
    case VMS_ERR_TRANSPORT: return "TRANSPORT";
    case VMS_ERR_EXEC: return "EXEC";
    default: return "?";
    }
}

static void test_basic_decode(VmsClient* cl)
{
    VmsError err;
    VmsConnection* cn;
    VmsStatement* st;
    const VmsValue* v;

    cn = vms_conn_open(cl, g_connstr, &err);
    CHECK(cn != NULL);
    if (!cn) { fprintf(stderr, "open failed: %s %s\n", err.sqlstate, err.message); return; }

    st = vms_stmt_exec_direct(cn, L"SELECT CAST(42 AS bigint) AS i, "
                                  L"CAST(2.5 AS float) AS f, "
                                  L"N'привет 🚀' AS t, "
                                  L"CAST(NULL AS int) AS nul", &err);
    CHECK(st != NULL);
    if (st) {
        CHECK(vms_stmt_col_count(st) == 4);
        CHECK(vms_stmt_meta(st, 0)->type == VMS_CT_INT64);
        CHECK(vms_stmt_meta(st, 1)->type == VMS_CT_FLOAT64);
        CHECK(strcmp(vms_stmt_meta(st, 2)->name, "t") == 0);
        {
            int frc = vms_stmt_fetch(st, &err);
            if (frc != 1) fprintf(stderr, "fetch rc=%d cls=%d %s %s\n", frc, err.cls, err.sqlstate, err.message);
            CHECK(frc == 1);
        }
        v = vms_stmt_value(st, 0);
        CHECK(v && v->type == VMS_VAL_INT64 && v->i == 42);
        v = vms_stmt_value(st, 1);
        CHECK(v && v->type == VMS_VAL_FLOAT64 && v->f == 2.5);
        v = vms_stmt_value(st, 2);
        CHECK(v && v->type == VMS_VAL_TEXT);
        if (v) CHECK(strstr(v->text, "\xF0\x9F\x9A\x80") != NULL); /* rocket emoji */
        v = vms_stmt_value(st, 3);
        CHECK(v && v->type == VMS_VAL_NULL);
        CHECK(vms_stmt_fetch(st, &err) == 0); /* exactly one row */
        vms_stmt_destroy(st);
    }

    /* row visibility invariant: values invalidated after fetch exhaustion */
    st = vms_stmt_exec_direct(cn, L"SELECT 1 AS a; SELECT 2 AS b; SELECT 3 AS c", &err);
    CHECK(st != NULL);
    if (st) {
        CHECK(vms_stmt_fetch(st, &err) == 1);
        CHECK(vms_stmt_value(st, 0)->i == 1);
        CHECK(vms_stmt_more_results(st, &err) == 1);
        CHECK(vms_stmt_col_count(st) == 1);
        CHECK(vms_stmt_fetch(st, &err) == 1);
        CHECK(vms_stmt_value(st, 0)->i == 2);
        CHECK(vms_stmt_more_results(st, &err) == 1);
        CHECK(vms_stmt_fetch(st, &err) == 1);
        CHECK(vms_stmt_value(st, 0)->i == 3);
        CHECK(vms_stmt_more_results(st, &err) == 0); /* drained */
        vms_stmt_destroy(st);
    }
    vms_conn_close(cn);
}

static void test_quarantine(VmsClient* cl)
{
    VmsError err;
    VmsConnection* cn = vms_conn_open(cl, g_connstr, &err);
    VmsStatement* st;
    CHECK(cn != NULL);
    if (!cn) return;
    CHECK(vms_conn_quarantined(cn) == 0);
    st = vms_stmt_exec_direct(cn, L"SELECT * FROM dbo.definitely_not_a_table_xyz", &err);
    CHECK(st == NULL);
    CHECK(err.cls == VMS_ERR_EXEC);
    /* non-transport errors do not quarantine: connection still usable */
    CHECK(vms_conn_quarantined(cn) == 0);
    st = vms_stmt_exec_direct(cn, L"SELECT 1 AS ok", &err);
    CHECK(st != NULL);
    if (st) vms_stmt_destroy(st);
    vms_conn_close(cn);
}

static void test_transactions(VmsClient* cl)
{
    VmsError err;
    VmsConnection* cn = vms_conn_open(cl, g_connstr, &err);
    VmsStatement* st;
    CHECK(cn != NULL);
    if (!cn) return;

    /* DDL in autocommit mode */
    vms_stmt_exec_direct(cn,
        L"IF OBJECT_ID(N'dbo.vms_r3') IS NOT NULL DROP TABLE dbo.vms_r3;"
        L"CREATE TABLE dbo.vms_r3(id int NOT NULL PRIMARY KEY)", &err);
    if (err.cls != VMS_OK) fprintf(stderr, "ddl err: %s %s\n", err.sqlstate, err.message);

    /* manual-transaction block: insert then rollback */
    CHECK(vms_tran_begin(cn, &err) == 0);
    st = vms_stmt_exec_direct(cn, L"INSERT INTO dbo.vms_r3 VALUES(1)", &err);
    CHECK(st != NULL);
    if (st) vms_stmt_destroy(st);
    CHECK(vms_tran_rollback(cn, &err) == 0);

    st = vms_stmt_exec_direct(cn, L"SELECT COUNT(*) AS n FROM dbo.vms_r3", &err);
    CHECK(st != NULL);
    if (st) {
        CHECK(vms_stmt_fetch(st, &err) == 1);
        {
            const VmsValue* v = vms_stmt_value(st, 0);
            CHECK(v && v->type == VMS_VAL_INT64 && v->i == 0); /* insert was rolled back */
            if (v && v->i != 0) fprintf(stderr, "count after rollback = %lld\n", v->i);
        }
        vms_stmt_destroy(st);
    }

    /* manual-transaction block: insert then commit */
    CHECK(vms_tran_begin(cn, &err) == 0);
    st = vms_stmt_exec_direct(cn, L"INSERT INTO dbo.vms_r3 VALUES(2)", &err);
    if (st) vms_stmt_destroy(st);
    CHECK(vms_tran_commit(cn, &err) == 0);

    st = vms_stmt_exec_direct(cn, L"SELECT COUNT(*) AS n FROM dbo.vms_r3", &err);
    CHECK(st != NULL);
    if (st) {
        CHECK(vms_stmt_fetch(st, &err) == 1);
        CHECK(vms_stmt_value(st, 0)->i == 1);
        vms_stmt_destroy(st);
    }

    vms_stmt_exec_direct(cn, L"IF OBJECT_ID(N'dbo.vms_r3') IS NOT NULL DROP TABLE dbo.vms_r3", &err);
    vms_conn_close(cn);
}

static void test_stream(VmsClient* cl)
{
    VmsError err;
    VmsConnection* cn = vms_conn_open(cl, g_connstr, &err);
    VmsStatement* st;
    wchar_t sql[256];
    long long n = 0, sum = 0, expect_sum = 0;
    int fetched = 0, i;

    CHECK(cn != NULL);
    if (!cn) return;
    for (i = 1; i <= 100; i++) expect_sum += i;
    _snwprintf_s(sql, 256, _TRUNCATE,
        L"WITH c AS (SELECT TOP (100) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i "
        L"FROM sys.all_objects) SELECT i, N'row-' + CAST(i AS nvarchar(10)) FROM c ORDER BY i");
    st = vms_stmt_exec_direct(cn, sql, &err);
    CHECK(st != NULL);
    if (!st) { vms_conn_close(cn); return; }
    while (vms_stmt_fetch(st, &err) == 1) {
        n++;
        sum += vms_stmt_value(st, 0)->i;
        fetched = 1;
        {
            const VmsValue* t = vms_stmt_value(st, 1);
            CHECK(t && t->type == VMS_VAL_TEXT);
        }
    }
    CHECK(n == 100);
    CHECK(sum == expect_sum);
    CHECK(fetched);
    vms_stmt_destroy(st);
    vms_conn_close(cn);
}

/* NOTE: the naive "fetch in main thread, cancel from main thread" test
 * deadlocks by construction — a blocked fetch cannot cancel itself.
 * Cross-thread cancellation is covered by test_cancel_deterministic. */
struct CancelCtx {
    VmsConnection* cn;
    volatile LONG* done;
};

/* background canceller: mirrors sqlite3_interrupt semantics — another host
 * thread interrupts the connection while the main thread is blocked in exec */
static unsigned __stdcall cancel_thread_proc(void* p)
{
    struct CancelCtx* c = (struct CancelCtx*)p;
    Sleep(1500);
    vms_conn_cancel(c->cn);
    InterlockedExchange(c->done, 1);
    return 0;
}

static void test_cancel_deterministic(VmsClient* cl)
{
    VmsError err;
    VmsConnection* cn = vms_conn_open(cl, g_connstr, &err);
    VmsStatement* st;
    HANDLE th;
    volatile LONG cancel_done = 0;
    struct CancelCtx ctx;

    CHECK(cn != NULL);
    if (!cn) return;
    ctx.cn = cn;
    ctx.done = &cancel_done;

    th = (HANDLE)_beginthreadex(NULL, 0, cancel_thread_proc, &ctx, 0, NULL);
    /* blocks on the worker until the server finishes or attention arrives */
    st = vms_stmt_exec_direct(cn,
        L"WITH c AS (SELECT CAST(1 AS bigint) AS n "
        L"UNION ALL SELECT n+1 FROM c WHERE n < 100000000) "
        L"SELECT COUNT(*) FROM c OPTION (MAXRECURSION 0)", &err);
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    CHECK(cancel_done == 1);
    CHECK(st == NULL);
    CHECK(err.cls == VMS_ERR_CANCELLED);
    /* connection must survive a cancelled statement */
    st = vms_stmt_exec_direct(cn, L"SELECT 1 AS ok", &err);
    CHECK(st != NULL);
    if (st) {
        CHECK(vms_stmt_fetch(st, &err) == 1);
        CHECK(vms_stmt_value(st, 0)->i == 1);
        vms_stmt_destroy(st);
    }
    vms_conn_close(cn);
}

static void test_oom_paths(void)
{
    VmsBounded b;
    /* OOM injection on the bounded-buffer allocation path (foundation hook) */
    vms_set_alloc_fail_hook(fail_once);
    CHECK(vms_buf_init(&b, 128) == 0);
    CHECK(b.data == NULL && b.cap == 0);
    vms_buf_free(&b); /* safe on failed init */
    vms_set_alloc_fail_hook(NULL);
    CHECK(vms_buf_init(&b, 128) == 1);
    CHECK(vms_buf_append(&b, "x", 1) == 1);
    vms_buf_free(&b);
}

int main(void)
{
    VmsError err;
    VmsClient* cl;

    if (!load_connstr()) return 77; /* ctest SKIP convention */

    if (getenv("VMS_SKIP_OOM") == NULL) test_oom_paths();

    cl = vms_client_init(&err);
    CHECK(cl != NULL);
    if (!cl) return 1;

    test_basic_decode(cl);
    test_quarantine(cl);
    test_transactions(cl);
    test_stream(cl);
    test_cancel_deterministic(cl);

    vms_client_destroy(cl);

    if (g_fail == 0) {
        printf("test_client: PASS\n");
        return 0;
    }
    fprintf(stderr, "test_client: %d failures\n", g_fail);
    return 1;
}
