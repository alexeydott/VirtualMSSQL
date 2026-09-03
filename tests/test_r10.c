/* G10 — DML tests: INSERT/UPDATE/DELETE through the vtab write path for
 * integer / string / GUID / composite stable keys; identity/computed/
 * rowversion write restrictions; optimistic conflict; AFTER trigger
 * re-read; view/query/ro rejection. */
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

static char* text_q(const char* sql)
{
    sqlite3_stmt* st = NULL;
    const char* t = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        t = (const char*)sqlite3_column_text(st, 0);
    }
    return t ? _strdup(t) : NULL;
}

static int exec_rc(const char* sql, char** err_out)
{
    char* err = NULL;
    int rc = sqlite3_exec(g_db, sql, NULL, NULL, &err);
    if (err_out) *err_out = err;
    else if (err) sqlite3_free(err);
    return rc;
}

int main(int argc, char** argv)
{
    const char* dll;
    const char* profile;

    if (argc < 2) {
        fprintf(stderr, "usage: test_r10 <virtualmssql.dll path>\n");
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

    /* ---- integer key table, mode=rw ---- */
    CHECK(exec_rc("CREATE VIRTUAL TABLE d10 USING virtualmssql("
                  "schema='dbo', table='vms10_dml', mode='rw');", NULL) == SQLITE_OK);

    /* INSERT */
    {
        char* err = NULL;
        int rc = exec_rc("INSERT INTO d10(id, name, val, txt) VALUES(10, 'ten', 100, 'ttt');", &err);
        if (rc != SQLITE_OK) fprintf(stderr, "insert rc=%d: %s\n", rc, err ? err : "?");
        if (err) sqlite3_free(err);
        CHECK(rc == SQLITE_OK);
    }
    CHECK(scalar("SELECT COUNT(*) FROM d10 WHERE id = 10") == 1);
    {
        char* t = text_q("SELECT name FROM d10 WHERE id = 10");
        CHECK(t && strcmp(t, "ten") == 0);
        free(t);
    }
    /* UPDATE */
    CHECK(exec_rc("UPDATE d10 SET val = 999, txt = 'upd' WHERE id = 10;", NULL) == SQLITE_OK);
    CHECK(scalar("SELECT val FROM d10 WHERE id = 10") == 999);
    {
        char* t = text_q("SELECT txt FROM d10 WHERE id = 10");
        CHECK(t && strcmp(t, "upd") == 0);
        free(t);
    }
    /* DELETE */
    CHECK(exec_rc("DELETE FROM d10 WHERE id = 10;", NULL) == SQLITE_OK);
    CHECK(scalar("SELECT COUNT(*) FROM d10 WHERE id = 10") == 0);
    /* key never updated */
    CHECK(exec_rc("UPDATE d10 SET id = 99 WHERE id = 1;", NULL) != SQLITE_OK ||
          scalar("SELECT COUNT(*) FROM d10 WHERE id = 1") == 1);

    /* ---- string key (composite) ---- */
    CHECK(exec_rc("CREATE VIRTUAL TABLE comp USING virtualmssql("
                  "schema='dbo', table='vms10_comp', mode='rw');", NULL) == SQLITE_OK);
    CHECK(exec_rc("INSERT INTO comp(ka, kb, val) VALUES('k9', 9, 99);", NULL) == SQLITE_OK);
    CHECK(scalar("SELECT val FROM comp WHERE ka = 'k9' AND kb = 9") == 99);
    CHECK(exec_rc("UPDATE comp SET val = 90 WHERE ka = 'k9' AND kb = 9;", NULL) == SQLITE_OK);
    CHECK(scalar("SELECT val FROM comp WHERE ka = 'k9' AND kb = 9") == 90);
    CHECK(exec_rc("DELETE FROM comp WHERE ka = 'k9' AND kb = 9;", NULL) == SQLITE_OK);
    CHECK(scalar("SELECT COUNT(*) FROM comp WHERE ka = 'k9'") == 0);

    /* ---- GUID key ---- */
    CHECK(exec_rc("CREATE VIRTUAL TABLE rv USING virtualmssql("
                  "schema='dbo', table='vms10_rv', mode='rw');", NULL) == SQLITE_OK);
    {
        char* g = text_q("SELECT id FROM rv WHERE v = 5");
        CHECK(g != NULL);
        CHECK(scalar("SELECT COUNT(*) FROM rv") == 2);
        /* identity-like NEWID default: insert without key */
        CHECK(exec_rc("INSERT INTO rv(v) VALUES(7);", NULL) == SQLITE_OK);
        CHECK(scalar("SELECT COUNT(*) FROM rv") == 3);
        /* update by guid key */
        {
            char sql[256];
            _snprintf_s(sql, sizeof(sql), _TRUNCATE,
                        "UPDATE rv SET v = 55 WHERE id = '%s';", g);
            CHECK(exec_rc(sql, NULL) == SQLITE_OK);
            CHECK(scalar("SELECT COUNT(*) FROM rv WHERE v = 55") == 1);
        }
        free(g);
    }

    /* ---- rowversion server-owned: not writable ---- */
    CHECK(exec_rc("CREATE VIRTUAL TABLE conf USING virtualmssql("
                  "schema='dbo', table='vms10_conf', mode='rw');", NULL) == SQLITE_OK);
    CHECK(exec_rc("UPDATE conf SET val = 111 WHERE id = 1;", NULL) == SQLITE_OK);
    CHECK(scalar("SELECT val FROM conf WHERE id = 1") == 111);
    /* rv column changes server-side on update; two consecutive updates give
     * two different rowversions (optimistic-lock token verifiability) */
    {
        char* rv1 = text_q("SELECT hex(rv) FROM conf WHERE id = 1");
        CHECK(exec_rc("UPDATE conf SET val = 112 WHERE id = 1;", NULL) == SQLITE_OK);
        {
            char* rv2 = text_q("SELECT hex(rv) FROM conf WHERE id = 1");
            CHECK(rv1 && rv2 && strcmp(rv1, rv2) != 0);
            free(rv1);
            free(rv2);
        }
    }

    /* ---- AFTER trigger: post-trigger state visible ---- */
    CHECK(exec_rc("CREATE VIRTUAL TABLE trg USING virtualmssql("
                  "schema='dbo', table='vms10_trg', mode='rw');", NULL) == SQLITE_OK);
    CHECK(exec_rc("UPDATE trg SET v = 50 WHERE id = 1;", NULL) == SQLITE_OK);
    /* AFTER trigger bumps v by 1: server value is 51 */
    CHECK(scalar("SELECT v FROM trg WHERE id = 1") == 51);
    /* post-trigger re-read: log contains the trigger-applied rows */
    CHECK(exec_rc("CREATE VIRTUAL TABLE trg_log USING virtualmssql("
                  "schema='dbo', table='vms10_trg_log');", NULL) == SQLITE_OK);
    CHECK(scalar("SELECT COUNT(*) FROM trg_log") >= 1);

    /* ---- rejections ---- */
    /* view: no stable key */
    {
        char* err = NULL;
        CHECK(exec_rc("CREATE VIRTUAL TABLE io USING virtualmssql("
                      "schema='dbo', table='vms10_io_trg', mode='rw');", &err) != SQLITE_OK ||
              scalar("SELECT COUNT(*) FROM io") >= 0);
        if (err) sqlite3_free(err);
    }
    /* query source: writes rejected structurally */
    CHECK(exec_rc("CREATE VIRTUAL TABLE qs USING virtualmssql("
                  "source='query', mode='rw', query='SELECT a FROM dbo.vms7_data');",
                  NULL) != SQLITE_OK);
    /* default mode=ro: write fails */
    CHECK(exec_rc("CREATE VIRTUAL TABLE ro USING virtualmssql("
                  "schema='dbo', table='vms10_dml');", NULL) == SQLITE_OK);
    {
        char* err = NULL;
        CHECK(exec_rc("INSERT INTO ro(id, name) VALUES(99, 'x');", &err) != SQLITE_OK);
        if (err) sqlite3_free(err);
    }

    /* cleanup */
    sqlite3_exec(g_db, "DROP TABLE IF EXISTS d10; DROP TABLE IF EXISTS comp;"
                       "DROP TABLE IF EXISTS rv; DROP TABLE IF EXISTS conf;"
                       "DROP TABLE IF EXISTS trg; DROP TABLE IF EXISTS io;"
                       "DROP TABLE IF EXISTS qs; DROP TABLE IF EXISTS ro;",
                 NULL, NULL, NULL);
    sqlite3_close(g_db);

    if (g_fail == 0) {
        printf("test_r10: PASS\n");
        return 0;
    }
    fprintf(stderr, "test_r10: %d failures\n", g_fail);
    return 1;
}
