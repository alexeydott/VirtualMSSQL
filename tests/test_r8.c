/* G8 — source=query tests: SELECT/CTE/JOIN/GROUP/window sources, validator
 * rejection matrix, metadata failure, result contract. Offline part covers
 * the lexer/validator; live part exercises the full vtab path. */
#include "vms_lexer.h"
#include "sqlite3.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { g_fail++; fprintf(stderr, "FAIL@%d: %s\n", __LINE__, #cond); } \
} while (0)

static void test_validator_offline(void)
{
    char err[256];

    /* accepted forms */
    CHECK(vms_tsql_validate_query(L"SELECT 1", err, sizeof(err)));
    CHECK(vms_tsql_validate_query(L"select a, b from dbo.t where a > 1", err, sizeof(err)));
    CHECK(vms_tsql_validate_query(L"WITH c AS (SELECT 1 AS x) SELECT x FROM c", err, sizeof(err)));
    CHECK(vms_tsql_validate_query(L"SELECT 1;", err, sizeof(err)));  /* trailing ; */
    CHECK(vms_tsql_validate_query(L"SELECT N'it''s -- not a comment' AS s", err, sizeof(err)));
    CHECK(vms_tsql_validate_query(L"SELECT 1 /* comment; select 2 */", err, sizeof(err)));
    CHECK(vms_tsql_validate_query(L"SELECT (SELECT COUNT(*) FROM t) AS n", err, sizeof(err)));

    /* rejected: statements that are not reads */
    CHECK(!vms_tsql_validate_query(L"INSERT INTO t VALUES(1)", err, sizeof(err)));
    CHECK(!vms_tsql_validate_query(L"UPDATE t SET a = 1", err, sizeof(err)));
    CHECK(!vms_tsql_validate_query(L"DELETE FROM t", err, sizeof(err)));
    CHECK(!vms_tsql_validate_query(L"EXEC dbo.sp_x", err, sizeof(err)));
    CHECK(!vms_tsql_validate_query(L"EXECUTE dbo.sp_x", err, sizeof(err)));
    CHECK(!vms_tsql_validate_query(L"CREATE TABLE t(a int)", err, sizeof(err)));
    CHECK(!vms_tsql_validate_query(L"DROP TABLE t", err, sizeof(err)));
    CHECK(!vms_tsql_validate_query(L"TRUNCATE TABLE t", err, sizeof(err)));
    CHECK(!vms_tsql_validate_query(L"SELECT * INTO newt FROM oldt", err, sizeof(err)));
    CHECK(!vms_tsql_validate_query(L"SET NOCOUNT ON; SELECT 1", err, sizeof(err)));
    CHECK(!vms_tsql_validate_query(L"BEGIN TRAN; SELECT 1", err, sizeof(err)));
    CHECK(!vms_tsql_validate_query(L"DECLARE @x int", err, sizeof(err)));
    CHECK(!vms_tsql_validate_query(L"SELECT 1; SELECT 2", err, sizeof(err))); /* multi */
    CHECK(!vms_tsql_validate_query(L"GRANT SELECT ON t TO x", err, sizeof(err)));
    CHECK(!vms_tsql_validate_query(L"SELECT * FROM OPENROWSET('x','y','z')", err, sizeof(err)));

    /* rejected: malformed */
    CHECK(!vms_tsql_validate_query(L"", err, sizeof(err)));
    CHECK(!vms_tsql_validate_query(L"SELECT 'unterminated", err, sizeof(err)));
    CHECK(!vms_tsql_validate_query(L"SELECT ((1)", err, sizeof(err)));
    CHECK(!vms_tsql_validate_query(L"SELECT 1))", err, sizeof(err)));
    CHECK(!vms_tsql_validate_query(L"x", err, sizeof(err)));
    CHECK(!vms_tsql_validate_query(L"/* unterminated comment", err, sizeof(err)));
}

/* ---- live part ---- */

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

static void test_live(void)
{
    char* sqle = NULL;

    /* plain SELECT source */
    {
        int rc = sqlite3_exec(g_db,
            "CREATE VIRTUAL TABLE q1 USING virtualmssql("
            "source='query', query='SELECT a, c FROM dbo.vms7_data WHERE a <= 8');",
            NULL, NULL, &sqle);
        if (rc != SQLITE_OK) fprintf(stderr, "q1 create rc=%d: %s\n", rc, sqle ? sqle : "?");
        CHECK(rc == SQLITE_OK);
    }
    CHECK(scalar("SELECT COUNT(*) FROM q1") == 7);
    CHECK(scalar("SELECT SUM(a) FROM q1") == 23); /* -3+1+2+3+5+7+8 */
    CHECK(scalar("SELECT COUNT(*) FROM q1 WHERE c IS NULL") == 3);

    /* CTE source */
    CHECK(sqlite3_exec(g_db,
        "CREATE VIRTUAL TABLE q2 USING virtualmssql("
        "source='query', query='WITH c AS (SELECT a, c FROM dbo.vms7_data WHERE c IS NOT NULL)"
        " SELECT a, c FROM c');",
        NULL, NULL, NULL) == SQLITE_OK);
    CHECK(scalar("SELECT COUNT(*) FROM q2") == 7);
    CHECK(scalar("SELECT SUM(c) FROM q2") == 660);

    /* JOIN source */
    CHECK(sqlite3_exec(g_db,
        "CREATE VIRTUAL TABLE q3 USING virtualmssql("
        "source='query', query='SELECT x.a, y.v FROM dbo.vms7_data x"
        " JOIN dbo.vms6_t_int y ON y.id = x.a');",
        NULL, NULL, NULL) == SQLITE_OK);
    CHECK(scalar("SELECT COUNT(*) FROM q3") == 3);
    CHECK(scalar("SELECT SUM(v) FROM q3") == 600); /* rows a=1,2,3 -> 100+200+300 */

    /* GROUP BY source */
    CHECK(sqlite3_exec(g_db,
        "CREATE VIRTUAL TABLE q4 USING virtualmssql("
        "source='query', query='SELECT COUNT(*) AS n, SUM(c) AS s FROM dbo.vms7_data');",
        NULL, NULL, NULL) == SQLITE_OK);
    CHECK(scalar("SELECT n FROM q4") == 12);
    CHECK(scalar("SELECT s FROM q4") == 660);

    /* window function source */
    CHECK(sqlite3_exec(g_db,
        "CREATE VIRTUAL TABLE q5 USING virtualmssql("
        "source='query', query='SELECT a, ROW_NUMBER() OVER (ORDER BY a) AS rn"
        " FROM dbo.vms7_data WHERE c IS NOT NULL');",
        NULL, NULL, NULL) == SQLITE_OK);
    CHECK(scalar("SELECT COUNT(*) FROM q5") == 7);
    CHECK(scalar("SELECT MAX(rn) FROM q5") == 7);

    /* local pushdown over query source stays correct */
    CHECK(scalar("SELECT COUNT(*) FROM q1 WHERE a = 5") == 1);
    CHECK(scalar("SELECT a FROM q2 ORDER BY a DESC LIMIT 1") == 20);

    /* rejection: validator refuses at CREATE time */
    CHECK(sqlite3_exec(g_db,
        "CREATE VIRTUAL TABLE qbad USING virtualmssql("
        "source='query', query='DELETE FROM dbo.vms7_data');",
        NULL, NULL, NULL) != SQLITE_OK);
    CHECK(sqlite3_exec(g_db,
        "CREATE VIRTUAL TABLE qbad USING virtualmssql("
        "source='query', query='SELECT 1; SELECT 2');",
        NULL, NULL, NULL) != SQLITE_OK);
    CHECK(sqlite3_exec(g_db,
        "CREATE VIRTUAL TABLE qbad USING virtualmssql("
        "source='query', query='EXEC dbo.nothing');",
        NULL, NULL, NULL) != SQLITE_OK);

    /* metadata failure: query references a missing table */
    CHECK(sqlite3_exec(g_db,
        "CREATE VIRTUAL TABLE qmeta USING virtualmssql("
        "source='query', query='SELECT a FROM dbo.no_such_table_9x');",
        NULL, NULL, NULL) != SQLITE_OK);

    /* result contract: duplicate column names rejected */
    CHECK(sqlite3_exec(g_db,
        "CREATE VIRTUAL TABLE qdup USING virtualmssql("
        "source='query', query='SELECT a, a FROM dbo.vms7_data');",
        NULL, NULL, NULL) != SQLITE_OK);

    /* cleanup */
    sqlite3_exec(g_db, "DROP TABLE IF EXISTS q1; DROP TABLE IF EXISTS q2;"
                       "DROP TABLE IF EXISTS q3; DROP TABLE IF EXISTS q4;"
                       "DROP TABLE IF EXISTS q5;", NULL, NULL, NULL);
}

int main(int argc, char** argv)
{
    const char* dll;
    const char* profile;

    test_validator_offline();

    if (argc < 2) {
        if (g_fail == 0) { printf("test_r8: PASS (offline)\n"); return 0; }
        fprintf(stderr, "test_r8: %d failures\n", g_fail);
        return 1;
    }
    dll = argv[1];
    profile = getenv("VMS_TEST_PROFILE");
    if (!profile || !profile[0]) {
        fprintf(stderr, "VMS_TEST_PROFILE not set; live tests skipped\n");
        if (g_fail == 0) { printf("test_r8: PASS (offline)\n"); return 0; }
        fprintf(stderr, "test_r8: %d failures\n", g_fail);
        return 1;
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
    test_live();
    sqlite3_close(g_db);

    if (g_fail == 0) {
        printf("test_r8: PASS\n");
        return 0;
    }
    fprintf(stderr, "test_r8: %d failures\n", g_fail);
    return 1;
}

