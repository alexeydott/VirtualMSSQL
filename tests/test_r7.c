/* G7 — differential tests: for every consumed pushdown operator the remote
 * (vtab) result must equal the SQLite-local result computed from a mirrored
 * local copy of the same rows.
 *
 * Operators: projection, EQ/LT/LE/GT/GE (int), IS NULL / IS NOT NULL,
 * single-value IN, ORDER BY int ASC/DESC, LIMIT, LIMIT+OFFSET. */
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

/* collect sorted row dump for a query; returns malloc'ed string */
static char* dump(const char* sql)
{
    sqlite3_stmt* st = NULL;
    char* out = NULL;
    size_t cap = 0, len = 0;
    int rc;

    rc = sqlite3_prepare_v2(g_db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "dump prepare failed (%s): %s\n", sql, sqlite3_errmsg(g_db));
        g_fail++;
        return _strdup("PREPARE_ERROR");
    }
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        char row[256];
        int n = sqlite3_column_count(st);
        int i;
        row[0] = 0;
        for (i = 0; i < n; i++) {
            const unsigned char* t = sqlite3_column_text(st, i);
            char item[128];
            _snprintf_s(item, sizeof(item), _TRUNCATE, "%s%s",
                        i ? "|" : "", t ? (const char*)t : "N");
            strncat_s(row, sizeof(row), item, _TRUNCATE);
        }
        strncat_s(row, sizeof(row), "\n", _TRUNCATE);
        if (len + strlen(row) + 1 > cap) {
            cap = (cap ? cap * 2 : 4096);
            out = (char*)realloc(out, cap);
        }
        memcpy(out + len, row, strlen(row) + 1);
        len += strlen(row);
    }
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "dump step failed (%s): %s\n", sql, sqlite3_errmsg(g_db));
        g_fail++;
    }
    sqlite3_finalize(st);
    if (!out) out = _strdup("");
    return out;
}

static void diff_check(const char* name, const char* remote_sql,
                       const char* local_sql)
{
    char* r = dump(remote_sql);
    char* l = dump(local_sql);
    if (strcmp(r, l) != 0) {
        g_fail++;
        fprintf(stderr, "DIFF [%s]\n--- remote ---\n%s--- local ---\n%s\n",
                name, r, l);
    } else {
        printf("ok: %s\n", name);
    }
    free(r);
    free(l);
}

int main(int argc, char** argv)
{
    const char* dll;
    const char* profile;

    if (argc < 2) {
        fprintf(stderr, "usage: test_r7 <virtualmssql.dll path>\n");
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
    CHECK(sqlite3_exec(g_db, "CREATE VIRTUAL TABLE d USING virtualmssql("
                             "schema='dbo', table='vms7_data');", NULL, NULL, NULL) == SQLITE_OK);

    /* local mirror of the remote rows for differential comparison */
    CHECK(sqlite3_exec(g_db,
        "CREATE TABLE local_d AS SELECT * FROM d;", NULL, NULL, NULL) == SQLITE_OK);

    /* ---- projection: subset of columns ---- */
    diff_check("projection subset",
        "SELECT a, c FROM d WHERE a <= 12 ORDER BY a",
        "SELECT a, c FROM local_d WHERE a <= 12 ORDER BY a");
    diff_check("projection all",
        "SELECT a, b, c FROM d WHERE a <= 5 ORDER BY a",
        "SELECT a, b, c FROM local_d WHERE a <= 5 ORDER BY a");

    /* ---- integer comparisons ---- */
    diff_check("EQ", "SELECT a FROM d WHERE a = 7 ORDER BY a",
                    "SELECT a FROM local_d WHERE a = 7 ORDER BY a");
    diff_check("LT", "SELECT a FROM d WHERE a < 5 ORDER BY a",
                    "SELECT a FROM local_d WHERE a < 5 ORDER BY a");
    diff_check("LE", "SELECT a FROM d WHERE a <= 5 ORDER BY a",
                    "SELECT a FROM local_d WHERE a <= 5 ORDER BY a");
    diff_check("GT", "SELECT a FROM d WHERE a > 15 ORDER BY a",
                    "SELECT a FROM local_d WHERE a > 15 ORDER BY a");
    diff_check("GE", "SELECT a FROM d WHERE a >= 15 ORDER BY a",
                    "SELECT a FROM local_d WHERE a >= 15 ORDER BY a");
    /* negative values */
    diff_check("EQ negative", "SELECT a FROM d WHERE a = -3 ORDER BY a",
                    "SELECT a FROM local_d WHERE a = -3 ORDER BY a");
    /* combined range */
    diff_check("range AND", "SELECT a FROM d WHERE a >= 3 AND a <= 8 ORDER BY a",
                    "SELECT a FROM local_d WHERE a >= 3 AND a <= 8 ORDER BY a");

    /* ---- IS NULL / IS NOT NULL ---- */
    diff_check("IS NULL", "SELECT a FROM d WHERE c IS NULL ORDER BY a",
                    "SELECT a FROM local_d WHERE c IS NULL ORDER BY a");
    diff_check("IS NOT NULL", "SELECT a, c FROM d WHERE c IS NOT NULL ORDER BY a",
                    "SELECT a, c FROM local_d WHERE c IS NOT NULL ORDER BY a");

    /* ---- single-value IN (pushed as equality) ---- */
    diff_check("IN single", "SELECT a FROM d WHERE a IN (7) ORDER BY a",
                    "SELECT a FROM local_d WHERE a IN (7) ORDER BY a");
    /* multi-value IN stays local — result must still be correct */
    diff_check("IN multi (local)", "SELECT a FROM d WHERE a IN (3, 7, 11) ORDER BY a",
                    "SELECT a FROM local_d WHERE a IN (3, 7, 11) ORDER BY a");

    /* ---- ORDER BY integer column ---- */
    diff_check("ORDER ASC", "SELECT a, c FROM d ORDER BY a",
                    "SELECT a, c FROM local_d ORDER BY a");
    diff_check("ORDER DESC", "SELECT a, c FROM d ORDER BY a DESC",
                    "SELECT a, c FROM local_d ORDER BY a DESC");
    diff_check("ORDER + WHERE", "SELECT a FROM d WHERE a >= 2 ORDER BY a DESC LIMIT 4",
                    "SELECT a FROM local_d WHERE a >= 2 ORDER BY a DESC LIMIT 4");

    /* ---- LIMIT / OFFSET ---- */
    diff_check("LIMIT", "SELECT a FROM d ORDER BY a LIMIT 3",
                    "SELECT a FROM local_d ORDER BY a LIMIT 3");
    diff_check("LIMIT+OFFSET", "SELECT a FROM d ORDER BY a LIMIT 5 OFFSET 3",
                    "SELECT a FROM local_d ORDER BY a LIMIT 5 OFFSET 3");
    diff_check("LIMIT no order", "SELECT a FROM d LIMIT 4",
                    "SELECT a FROM local_d LIMIT 4");
    diff_check("OFFSET beyond", "SELECT a FROM d ORDER BY a LIMIT 5 OFFSET 500",
                    "SELECT a FROM local_d ORDER BY a LIMIT 5 OFFSET 500");

    /* ---- combined: WHERE + ORDER + LIMIT ---- */
    diff_check("combined", "SELECT a, c FROM d WHERE a > 1 AND c IS NOT NULL "
                           "ORDER BY a DESC LIMIT 3 OFFSET 1",
                    "SELECT a, c FROM local_d WHERE a > 1 AND c IS NOT NULL "
                    "ORDER BY a DESC LIMIT 3 OFFSET 1");

    /* ---- text comparison stays local (correctness, not pushdown) ---- */
    diff_check("text compare local", "SELECT a FROM d WHERE b > 'm' ORDER BY a",
                    "SELECT a FROM local_d WHERE b > 'm' ORDER BY a");

    /* ---- aggregates over pushdown ---- */
    diff_check("aggregate", "SELECT COUNT(*), SUM(a) FROM d WHERE a <= 10",
                    "SELECT COUNT(*), SUM(a) FROM local_d WHERE a <= 10");

    if (g_fail == 0) {
        printf("test_r7: PASS\n");
        sqlite3_close(g_db);
        return 0;
    }
    fprintf(stderr, "test_r7: %d failures\n", g_fail);
    sqlite3_close(g_db);
    return 1;
}
