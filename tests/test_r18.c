/* G18 addendum — remote schema inspection table functions + public ABI.
 *
 * Uses VMS_TEST_PROFILE as the connection specification (same server as
 * other suites), so the seed fixtures are directly inspectable. */
#include "sqlite3.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_fail = 0;
static sqlite3* g_db = NULL;
static const char* g_conn = NULL;

#define CHECK(cond) do { \
    if (!(cond)) { g_fail++; fprintf(stderr, "FAIL@%d: %s\n", __LINE__, #cond); } \
} while (0)

static long long scalar_q(const char* fmt, const char* arg)
{
    sqlite3_stmt* st = NULL;
    char sql[2048];
    long long v = -1;
    _snprintf_s(sql, sizeof(sql), _TRUNCATE, fmt, g_conn, arg);
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        v = sqlite3_column_int64(st, 0);
    }
    if (st) sqlite3_finalize(st);
    return v;
}

static char* text_q(const char* fmt, const char* arg)
{
    sqlite3_stmt* st = NULL;
    char sql[2048];
    const char* t = NULL;
    char* out = NULL;
    _snprintf_s(sql, sizeof(sql), _TRUNCATE, fmt, g_conn, arg);
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "text_q prepare: %s\n", sqlite3_errmsg(g_db));
        return NULL;
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
        t = (const char*)sqlite3_column_text(st, 0);
        out = t ? _strdup(t) : NULL;
    } else {
        fprintf(stderr, "text_q: no row for %s\n", sql);
    }
    sqlite3_finalize(st);
    return out;
}

static int exec_rc(const char* sql)
{
    return sqlite3_exec(g_db, sql, NULL, NULL, NULL);
}

int main(int argc, char** argv)
{
    const char* dll;
    const char* profile;

    if (argc < 2) {
        fprintf(stderr, "usage: test_r18 <virtualmssql.dll>\n");
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
    CHECK(sqlite3_exec(g_db, "SELECT virtualmssql_cred('test:uid', 'sa')", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_exec(g_db, "SELECT virtualmssql_cred('test:pwd', 'Vms-Probe-2026!x')", NULL, NULL, NULL) == SQLITE_OK);
    g_conn = profile;

    /* ---- ABI: api_version ---- */
    CHECK(scalar_q("SELECT virtualmssql_api_version()", "") == 1);

    /* ---- ABI: wincred_provider is available ---- */
    CHECK(scalar_q("SELECT virtualmssql_wincred_provider()", "") == 1);

    /* ---- ABI: invalid credential provider rejected ---- */
    CHECK(exec_rc(
        "SELECT virtualmssql_register_credential_provider(NULL)") != SQLITE_OK);

    /* ---- tables ---- */
    {
        char* t = text_q(
            "SELECT table_name FROM virtualmssql_tables('%s', 'dbo')"
            " WHERE table_name = 'vms10_dml'", "dbo");
        CHECK(t && strcmp(t, "vms10_dml") == 0);
        if (t) free(t);
        t = text_q(
            "SELECT table_type FROM virtualmssql_tables('%s', 'dbo')"
            " WHERE table_name = 'vms6_view'", "dbo");
        CHECK(t && strcmp(t, "view") == 0);
        if (t) free(t);
    }

    /* ---- table_info: PRAGMA table_info contract ---- */
    {
        char* t;
        long long cid, pk, hidden;
        t = text_q(
            "SELECT name FROM virtualmssql_table_info('%s', 'dbo', 'vms10_dml')"
            " WHERE cid = 0", "dbo");
        CHECK(t && strcmp(t, "id") == 0);
        if (t) free(t);
        cid = scalar_q(
            "SELECT cid FROM virtualmssql_table_info('%s', 'dbo', 'vms10_dml')"
            " WHERE name = 'id'", "dbo");
        CHECK(cid == 0);
        pk = scalar_q(
            "SELECT pk FROM virtualmssql_table_info('%s', 'dbo', 'vms10_dml')"
            " WHERE name = 'id'", "dbo");
        CHECK(pk == 1);
        hidden = scalar_q(
            "SELECT hidden_flags FROM virtualmssql_table_info('%s', 'dbo', 'vms10_dml')"
            " WHERE name = 'id'", "dbo");
        CHECK(hidden == 0);
        t = text_q(
            "SELECT type FROM virtualmssql_table_info('%s', 'dbo', 'vms10_dml')"
            " WHERE name = 'name'", "dbo");
        CHECK(t && strcmp(t, "nvarchar") == 0);
        if (t) free(t);
    }

    /* ---- index_list: PK origin ---- */
    {
        char* t = text_q(
            "SELECT origin FROM virtualmssql_index_list('%s', 'dbo', 'vms10_dml')"
            " WHERE name LIKE 'PK%%'", "dbo");
        CHECK(t && strcmp(t, "pk") == 0);
        if (t) free(t);
    }

    /* ---- index_info: PK key column ---- */
    {
        sqlite3_stmt* st = NULL;
        char* t = NULL;
        char q2[2048];
        _snprintf_s(q2, sizeof(q2), _TRUNCATE,
            "SELECT name FROM virtualmssql_index_info("
            "'%s', 'dbo', 'vms10_dml',"
            " (SELECT name FROM virtualmssql_index_list("
            "  '%s', 'dbo', 'vms10_dml') WHERE origin = 'pk'))"
            " WHERE seqno = 1", g_conn, g_conn);
        CHECK(sqlite3_prepare_v2(g_db, q2, -1, &st, NULL) == SQLITE_OK);
        CHECK(sqlite3_step(st) == SQLITE_ROW);
        t = (char*)sqlite3_column_text(st, 0);
        CHECK(t && strcmp(t, "id") == 0);
        sqlite3_finalize(st);
    }

    /* ---- injection resistance ---- */
    CHECK(exec_rc(
        "SELECT * FROM virtualmssql_columns("
        "'server=x;auth=sql;cred=test;tls=trust',"
        " 'dbo', 'vms10_dml''; DROP TABLE vms10_dml--')") != SQLITE_OK);

    sqlite3_close(g_db);

    if (g_fail == 0) {
        printf("test_r18: PASS\n");
        return 0;
    }
    fprintf(stderr, "test_r18: %d failures\n", g_fail);
    return 1;
}
