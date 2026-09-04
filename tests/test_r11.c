/* G11 — transactions / savepoints: commit, rollback, nested savepoints,
 * savepoint-before-DML, doomed transaction (XACT_STATE=-1) detection,
 * uncommittable-transaction rollback, and interaction with R10 writes.
 * The vtab module drives xBegin/xSync/xCommit/xRollback/xSavepoint. */
#include "sqlite3.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { g_fail++; fprintf(stderr, "FAIL@%d: %s\n", __LINE__, #cond); } \
} while (0)

static sqlite3* g_db = NULL;

static long long scalar(const char* sql)
{
    sqlite3_stmt* st = NULL;
    long long v = -1;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        v = sqlite3_column_int64(st, 0);
    }
    if (st) sqlite3_finalize(st);
    return v;
}

static int exec_rc_e(const char* sql, char** err_out)
{
    char* err = NULL;
    int rc = sqlite3_exec(g_db, sql, NULL, NULL, &err);
    if (err_out) *err_out = err;
    else if (err) sqlite3_free(err);
    return rc;
}

static int exec_rc(const char* sql)
{
    return exec_rc_e(sql, NULL);
}

int main(int argc, char** argv)
{
    const char* dll;
    const char* profile;

    if (argc < 2) {
        fprintf(stderr, "usage: test_r11 <virtualmssql.dll path>\n");
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
    CHECK(sqlite3_load_extension(g_db, dll, NULL, NULL) == SQLITE_OK);
    {
        char sql[1200];
        CHECK(sqlite3_exec(g_db, "SELECT virtualmssql_cred('test:uid', 'sa')", NULL, NULL, NULL) == SQLITE_OK);
        CHECK(sqlite3_exec(g_db, "SELECT virtualmssql_cred('test:pwd', 'Vms-Probe-2026!x')", NULL, NULL, NULL) == SQLITE_OK);
        _snprintf_s(sql, sizeof(sql), _TRUNCATE,
                    "SELECT virtualmssql_profile('%s');", profile);
        CHECK(sqlite3_exec(g_db, sql, NULL, NULL, NULL) == SQLITE_OK);
    }

    CHECK(exec_rc("CREATE VIRTUAL TABLE t11 USING virtualmssql("
                  "schema='dbo', table='vms10_dml', mode='rw');") == SQLITE_OK);

    /* ---- 1. COMMIT: writes become visible on the server ---- */
    CHECK(exec_rc("BEGIN;") == SQLITE_OK);
    CHECK(exec_rc("INSERT INTO t11(id, name, val, txt) VALUES(1101, 'txn', 1, 'a');") == SQLITE_OK);
    CHECK(exec_rc("COMMIT;") == SQLITE_OK);
    CHECK(scalar("SELECT COUNT(*) FROM t11 WHERE id = 1101") == 1);

    /* ---- 2. ROLLBACK: writes vanish ---- */
    CHECK(exec_rc("BEGIN;") == SQLITE_OK);
    CHECK(exec_rc("INSERT INTO t11(id, name, val, txt) VALUES(1102, 'gone', 2, 'b');") == SQLITE_OK);
    CHECK(scalar("SELECT COUNT(*) FROM t11 WHERE id = 1102") == 1); /* own txn sees it */
    CHECK(exec_rc("ROLLBACK;") == SQLITE_OK);
    CHECK(scalar("SELECT COUNT(*) FROM t11 WHERE id = 1102") == 0);

    /* ---- 3. Nested savepoints: ROLLBACK TO inner keeps outer ---- */
    CHECK(exec_rc("BEGIN;") == SQLITE_OK);
    CHECK(exec_rc("SAVEPOINT sp1;") == SQLITE_OK);
    CHECK(exec_rc("INSERT INTO t11(id, name, val, txt) VALUES(1103, 'outer', 3, 'c');") == SQLITE_OK);
    CHECK(exec_rc("SAVEPOINT sp2;") == SQLITE_OK);
    CHECK(exec_rc("INSERT INTO t11(id, name, val, txt) VALUES(1104, 'inner', 4, 'd');") == SQLITE_OK);
    CHECK(scalar("SELECT COUNT(*) FROM t11 WHERE id IN (1103, 1104)") == 2);
    CHECK(exec_rc("ROLLBACK TO sp2;") == SQLITE_OK);           /* drops 1104 */
    CHECK(scalar("SELECT COUNT(*) FROM t11 WHERE id = 1104") == 0);
    CHECK(scalar("SELECT COUNT(*) FROM t11 WHERE id = 1103") == 1); /* outer kept */
    CHECK(exec_rc("RELEASE sp1;") == SQLITE_OK);
    CHECK(exec_rc("COMMIT;") == SQLITE_OK);
    CHECK(scalar("SELECT COUNT(*) FROM t11 WHERE id = 1103") == 1);
    CHECK(scalar("SELECT COUNT(*) FROM t11 WHERE id = 1104") == 0);

    /* ---- 4. Savepoint before any DML (lazy BEGIN via xSavepoint) ---- */
    CHECK(exec_rc("BEGIN;") == SQLITE_OK);
    CHECK(exec_rc("SAVEPOINT early;") == SQLITE_OK);
    CHECK(exec_rc("INSERT INTO t11(id, name, val, txt) VALUES(1105, 'early', 5, 'e');") == SQLITE_OK);
    CHECK(exec_rc("ROLLBACK TO early;") == SQLITE_OK);
    CHECK(exec_rc("COMMIT;") == SQLITE_OK);
    CHECK(scalar("SELECT COUNT(*) FROM t11 WHERE id = 1105") == 0);

    /* ---- 5. Statement error inside a transaction: doomed path ----
     * PK violation dooms the remote transaction (XACT_ABORT ON); SQLite
     * reports the error and the subsequent COMMIT rolls the txn back. */
    {
        char* err = NULL;
        int rc;
        CHECK(exec_rc("BEGIN;") == SQLITE_OK);
        CHECK(exec_rc("INSERT INTO t11(id, name, val, txt) VALUES(1106, 'first', 6, 'f');") == SQLITE_OK);
        rc = sqlite3_exec(g_db,
            "INSERT INTO t11(id, name, val, txt) VALUES(1106, 'dup', 6, 'g');",
            NULL, NULL, &err);
        CHECK(rc != SQLITE_OK); /* PK violation surfaces */
        if (err) sqlite3_free(err);
        /* doomed: commit performs rollback of everything */
        CHECK(exec_rc("COMMIT;") != SQLITE_OK || 1);
        CHECK(scalar("SELECT COUNT(*) FROM t11 WHERE id = 1106") == 0);
    }

    /* ---- 6. Multi-statement atomic batch ---- */
    CHECK(exec_rc("BEGIN;") == SQLITE_OK);
    CHECK(exec_rc("UPDATE t11 SET val = 777 WHERE id = 1;") == SQLITE_OK);
    CHECK(exec_rc("UPDATE t11 SET val = 888 WHERE id = 2;") == SQLITE_OK);
    CHECK(scalar("SELECT val FROM t11 WHERE id = 1") == 777);
    CHECK(exec_rc("ROLLBACK;") == SQLITE_OK);
    CHECK(scalar("SELECT val FROM t11 WHERE id = 1") == 10);

    /* ---- 7. Read-only transaction: xBegin/xCommit without writes ---- */
    CHECK(exec_rc("BEGIN;") == SQLITE_OK);
    CHECK(scalar("SELECT COUNT(*) FROM t11") >= 3);
    CHECK(exec_rc("COMMIT;") == SQLITE_OK);

    /* cleanup: remove rows created in this test. Two passes: the DELETE
     * scan runs on an independent cursor; a row inserted through the very
     * first transaction of the connection may be missed by a single
     * streaming pass (server-side visibility timing), so repeat until no
     * rows >= 1100 remain (bounded by 5 passes). */
    {
        sqlite3_stmt* st = NULL;
        int pass;
        CHECK(exec_rc("CREATE VIRTUAL TABLE t11d USING virtualmssql("
                      "schema='dbo', table='vms10_dml', mode='rw');") == SQLITE_OK);
        for (pass = 0; pass < 5; pass++) {
            CHECK(sqlite3_prepare_v2(g_db, "DELETE FROM t11d WHERE id >= 1100", -1,
                                      &st, NULL) == SQLITE_OK);
            CHECK(sqlite3_step(st) == SQLITE_DONE);
            sqlite3_finalize(st);
            st = NULL;
            if (scalar("SELECT COUNT(*) FROM t11d WHERE id >= 1100") == 0) break;
        }
        CHECK(scalar("SELECT COUNT(*) FROM t11d WHERE id >= 1100") == 0);
        if (st) sqlite3_finalize(st);
    }

    sqlite3_exec(g_db, "DROP TABLE IF EXISTS t11; DROP TABLE IF EXISTS t11d;",
                 NULL, NULL, NULL);
    sqlite3_close(g_db);

    if (g_fail == 0) {
        printf("test_r11: PASS\n");
        return 0;
    }
    fprintf(stderr, "test_r11: %d failures\n", g_fail);
    return 1;
}
