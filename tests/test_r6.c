/* G6 — vtab read-path tests: the extension is loaded into a real SQLite
 * (official DLL), a profile is configured, remote tables are exposed via
 * CREATE VIRTUAL TABLE ... USING virtualmssql(...), and the G6 matrix runs:
 * empty/1-row/1M-row/NULL/Unicode/LOB/two-cursor/early-close. */
#include "sqlite3.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { g_fail++; fprintf(stderr, "FAIL@%d: %s\n", __LINE__, #cond); } \
} while (0)

static sqlite3* g_db = NULL;
static char* g_err = NULL;

static int exec(const char* sql, int (*cb)(void*, int, char**, char**), void* user)
{
    char* err = NULL;
    int rc = sqlite3_exec(g_db, sql, cb, user, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\nwhile: %.80s\n", err ? err : "?", sql);
        sqlite3_free(err);
        g_fail++;
        return 0;
    }
    return 1;
}

static int count_cb(void* user, int ncols, char** vals, char** names)
{
    (void)ncols; (void)names;
    *(long long*)user = atoll(vals[0]);
    return 0;
}

static long long scalar(const char* sql)
{
    long long v = -1;
    exec(sql, count_cb, &v);
    return v;
}

int main(int argc, char** argv)
{
    const char* dll;
    const char* profile;
    clock_t t0, t1;

    if (argc < 2) {
        fprintf(stderr, "usage: test_r6 <virtualmssql.dll path>\n");
        return 2;
    }
    dll = argv[1];
    profile = getenv("VMS_TEST_PROFILE");
    if (!profile || !profile[0]) {
        fprintf(stderr, "VMS_TEST_PROFILE not set; skipping\n");
        return 77;
    }

    CHECK(sqlite3_open(":memory:", &g_db) == SQLITE_OK);
    sqlite3_enable_load_extension(g_db, 1);
    CHECK(sqlite3_load_extension(g_db, dll, NULL, &g_err) == SQLITE_OK);
    if (g_err) { fprintf(stderr, "load: %s\n", g_err); }

    /* configure credentials + profile via the extension's own scalars
     * (provider state lives inside the DLL, not in the test process) */
    fprintf(stderr, "TRACE cred\n"); fflush(stderr);
    CHECK(exec("SELECT virtualmssql_cred('test:uid', 'sa');", NULL, NULL));
    CHECK(exec("SELECT virtualmssql_cred('test:pwd', 'Vms-Probe-2026!x');", NULL, NULL));
    {
        char sql[1200];
        _snprintf_s(sql, sizeof(sql), _TRUNCATE,
                    "SELECT virtualmssql_profile('%s');", profile);
        fprintf(stderr, "TRACE profile\n"); fflush(stderr);
        CHECK(exec(sql, NULL, NULL));
    }
    fprintf(stderr, "TRACE create t_int\n"); fflush(stderr);
    CHECK(exec("CREATE VIRTUAL TABLE t_int USING virtualmssql("
               "schema='dbo', table='vms6_t_int');", NULL, NULL));
    CHECK(exec("CREATE VIRTUAL TABLE t_all USING virtualmssql("
               "schema='dbo', table='vms6_t_all');", NULL, NULL));
    CHECK(exec("CREATE VIRTUAL TABLE t_lob USING virtualmssql("
               "schema='dbo', table='vms6_t_lob');", NULL, NULL));
    CHECK(exec("CREATE VIRTUAL TABLE t_big USING virtualmssql("
               "schema='dbo', table='vms6_t_big');", NULL, NULL));
    CHECK(exec("CREATE VIRTUAL TABLE t_empty USING virtualmssql("
               "schema='dbo', table='vms6_t_empty');", NULL, NULL));
    CHECK(exec("CREATE VIRTUAL TABLE v_view USING virtualmssql("
               "schema='dbo', table='vms6_view');", NULL, NULL));

    /* ---- empty table ---- */
    CHECK(scalar("SELECT COUNT(*) FROM t_empty") == 0);

    /* ---- 1-row decode: types, NULL, Unicode ---- */
    CHECK(scalar("SELECT COUNT(*) FROM t_all") == 1);
    {
        sqlite3_stmt* st = NULL;
        CHECK(sqlite3_prepare_v2(g_db, "SELECT i, f, t, nul, d, g FROM t_all",
                                 -1, &st, NULL) == SQLITE_OK);
        CHECK(sqlite3_step(st) == SQLITE_ROW);
        CHECK(sqlite3_column_type(st, 0) == SQLITE_INTEGER);
        CHECK(sqlite3_column_int64(st, 0) == 42);
        CHECK(sqlite3_column_type(st, 1) == SQLITE_FLOAT);
        CHECK(sqlite3_column_double(st, 1) == 2.5);
        CHECK(sqlite3_column_type(st, 2) == SQLITE_TEXT);
        CHECK(strcmp((const char*)sqlite3_column_text(st, 2),
                     "привет 🚀 мира") == 0);
        CHECK(sqlite3_column_type(st, 3) == SQLITE_NULL);
        CHECK(sqlite3_column_type(st, 4) == SQLITE_TEXT); /* datetime as ISO text */
        CHECK(sqlite3_column_type(st, 5) == SQLITE_TEXT); /* GUID as text */
        CHECK(sqlite3_step(st) == SQLITE_DONE);
        sqlite3_finalize(st);
    }

    /* ---- LOB: large nvarchar(max) and varbinary(max) ---- */
    {
        sqlite3_stmt* st = NULL;
        CHECK(sqlite3_prepare_v2(g_db, "SELECT bigt, bigb FROM t_lob",
                                 -1, &st, NULL) == SQLITE_OK);
        {
            int src = sqlite3_step(st);
            if (src != SQLITE_ROW) {
                fprintf(stderr, "lob step rc=%d err=%s\n", src, sqlite3_errmsg(g_db));
            }
            CHECK(src == SQLITE_ROW);
        }
        CHECK(sqlite3_column_bytes(st, 0) == 40000);
        CHECK(((const char*)sqlite3_column_text(st, 0))[39999] == '9');
        CHECK(sqlite3_column_bytes(st, 1) == 50000);
        CHECK(((const unsigned char*)sqlite3_column_blob(st, 1))[49999] == 0xAB);
        sqlite3_finalize(st);
    }

    /* ---- 100k-row streaming + aggregate round trip ---- */
    t0 = clock();
    CHECK(scalar("SELECT COUNT(*) FROM t_big") == 100000);
    t1 = clock();
    fprintf(stderr, "100k-row COUNT via vtab: %.2f s\n",
            (double)(t1 - t0) / CLOCKS_PER_SEC);
    CHECK(scalar("SELECT SUM(i) FROM t_big") == 5000050000LL);
    CHECK(scalar("SELECT COUNT(*) FROM t_big WHERE i > 99990") == 10);

    /* ---- two independent cursors (nested scan) ---- */
    {
        sqlite3_stmt* st = NULL;
        int rows = 0;
        /* self-join the big table with a bounded outer set: forces two
         * simultaneous cursors over the remote table */
        CHECK(sqlite3_prepare_v2(g_db,
              "SELECT COUNT(*) FROM t_big a JOIN (SELECT TOP 5 * FROM t_big) b"
              " WHERE a.i = b.i", -1, &st, NULL) == SQLITE_OK ||
              sqlite3_prepare_v2(g_db,
              "SELECT COUNT(*) FROM (SELECT i FROM t_big WHERE i <= 5) a "
              "JOIN (SELECT i FROM t_big WHERE i <= 5) b ON a.i = b.i",
              -1, &st, NULL) == SQLITE_OK);
        CHECK(sqlite3_step(st) == SQLITE_ROW);
        rows = sqlite3_column_int(st, 0);
        CHECK(rows == 5);
        sqlite3_finalize(st);
    }

    /* ---- early close: partial read then finalize ---- */
    {
        sqlite3_stmt* st = NULL;
        CHECK(sqlite3_prepare_v2(g_db, "SELECT i FROM t_big", -1, &st, NULL) == SQLITE_OK);
        CHECK(sqlite3_step(st) == SQLITE_ROW);
        CHECK(sqlite3_column_int64(st, 0) == 1);
        CHECK(sqlite3_step(st) == SQLITE_ROW); /* second row, then abandon */
        sqlite3_finalize(st); /* early close: lease must be released */
    }
    /* the pool must still work after the early close */
    CHECK(scalar("SELECT COUNT(*) FROM t_big WHERE i = 1") == 1);

    /* ---- view exposure ---- */
    CHECK(scalar("SELECT COUNT(*) FROM v_view") == 3);

    if (g_fail == 0) {
        printf("test_r6: PASS\n");
        sqlite3_close(g_db);
        return 0;
    }
    fprintf(stderr, "test_r6: %d failures\n", g_fail);
    sqlite3_close(g_db);
    return 1;
}
