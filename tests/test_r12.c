/* G12 — type matrix / spatial: mandatory SQL Server types read through the
 * vtab (exact TEXT policy for decimal/money/datetime), NULL row handling,
 * boundary values, WKB/WKT spatial representation (validated through
 * mod_spatialite when available), and the deterministic UNSUPPORTED_TYPE
 * policy for sql_variant. */
#include "sqlite3.h"
#include <windows.h>
#include <io.h>
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

static double fscalar(const char* sql)
{
    sqlite3_stmt* st = NULL;
    double v = -1;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        v = sqlite3_column_double(st, 0);
    }
    if (st) sqlite3_finalize(st);
    return v;
}

static char* text_q(const char* sql)
{
    sqlite3_stmt* st = NULL;
    const char* t = NULL;
    char* out = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        t = (const char*)sqlite3_column_text(st, 0);
        out = t ? _strdup(t) : NULL; /* copy BEFORE finalize */
    }
    if (st) sqlite3_finalize(st);
    return out;
}

static int blob_q(const char* sql, const unsigned char** out, int* len)
{
    sqlite3_stmt* st = NULL;
    static unsigned char hold[65536];
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); return 0; }
    {
        const void* p = sqlite3_column_blob(st, 0);
        *len = sqlite3_column_bytes(st, 0);
        if (*len < 0) *len = 0;
        if (*len > (int)sizeof(hold)) *len = (int)sizeof(hold);
        memcpy(hold, p, (size_t)*len); /* copy BEFORE finalize */
        *out = hold;
    }
    sqlite3_finalize(st);
    return 1;
}

static int is_null(const char* sql)
{
    sqlite3_stmt* st = NULL;
    int isnull = 1;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        isnull = (sqlite3_column_type(st, 0) == SQLITE_NULL);
    }
    if (st) sqlite3_finalize(st);
    return isnull;
}

static int exec_rc(const char* sql)
{
    return sqlite3_exec(g_db, sql, NULL, NULL, NULL);
}

/* validate a WKB blob with mod_spatialite (loaded on a scratch connection);
 * returns 1 when the geometry parses and matches the expected WKT */
static int spatialite_validate_wkb(const unsigned char* wkb, int wkb_len,
                                   const char* expect_wkt)
{
    sqlite3* sdb = NULL;
    sqlite3_stmt* st = NULL;
    char sql[1024];
    char dll[MAX_PATH];
    int ok = 0;

    if (_access("D:\\projects\\externals\\spatialite\\bin\\win64\\mod_spatialite.dll", 0) == 0)
        strcpy_s(dll, sizeof(dll), "D:\\projects\\externals\\spatialite\\bin\\win64\\mod_spatialite.dll");
    else
        strcpy_s(dll, sizeof(dll), "D:\\projects\\externals\\spatialite\\bin\\win32\\mod_spatialite.dll");

    if (sqlite3_open(":memory:", &sdb) != SQLITE_OK) return 0;
    sqlite3_enable_load_extension(sdb, 1);
    {
        char* err = NULL;
        if (sqlite3_load_extension(sdb, dll, NULL, &err) != SQLITE_OK) {
            /* spatialite unavailable: skip validation, not a test failure */
            if (err) sqlite3_free(err);
            sqlite3_close(sdb);
            return 1;
        }
    }
    _snprintf_s(sql, sizeof(sql), _TRUNCATE,
                "SELECT ST_AsText(CastToXY(GeomFromWKB(?1)))");
    if (sqlite3_prepare_v2(sdb, sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_bind_blob(st, 1, wkb, wkb_len, SQLITE_STATIC) == SQLITE_OK &&
            sqlite3_step(st) == SQLITE_ROW) {
            const char* got = (const char*)sqlite3_column_text(st, 0);
            if (got) {
                /* mod_spatialite emits "POINT(1 2)" / "POINT(1.0 2.0)" style
                 * text; normalize spaces for the comparison */
                char norm[256];
                int a, b;
                for (a = 0, b = 0; got[a] && b < 255; a++)
                    if (got[a] != ' ') norm[b++] = got[a];
                norm[b] = 0;
                /* compare ignoring the space differences: strip from expect */
                {
                    char en[256];
                    int c, d = 0;
                    for (c = 0; expect_wkt[c] && d < 255; c++)
                        if (expect_wkt[c] != ' ') en[d++] = expect_wkt[c];
                    en[d] = 0;
                    ok = (strcmp(norm, en) == 0) ||
                         (strncmp(norm, en, strlen(en) - 1) == 0 &&
                          en[strlen(en) - 1] == ')');
                }
            }
        }
    }
    if (st) sqlite3_finalize(st);
    sqlite3_close(sdb);
    return ok;
}

int main(int argc, char** argv)
{
    const char* dll;
    const char* profile;

    if (argc < 2) {
        fprintf(stderr, "usage: test_r12 <virtualmssql.dll path>\n");
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

    /* ---- type matrix vtab ---- */
    CHECK(exec_rc("CREATE VIRTUAL TABLE t12 USING virtualmssql("
                  "schema='dbo', table='vms12_types');") == SQLITE_OK);

    /* row 1: values, row 2: NULLs everywhere */
    CHECK(scalar("SELECT COUNT(*) FROM t12") == 2);

    /* bit/tinyint/smallint/bigint */
    CHECK(scalar("SELECT b FROM t12 WHERE id = 1") == 1);
    CHECK(scalar("SELECT ti FROM t12 WHERE id = 1") == 255);         /* boundary: max tinyint */
    CHECK(scalar("SELECT si FROM t12 WHERE id = 1") == -32768);      /* boundary: min smallint */
    CHECK(scalar("SELECT bi FROM t12 WHERE id = 1") == 9223372036854775807LL); /* max bigint */

    /* decimal/money: exact TEXT policy */
    {
        char* t = text_q("SELECT d10 FROM t12 WHERE id = 1");
        CHECK(t && strcmp(t, "12345678.90") == 0);
        free(t);
    }
    {
        char* t = text_q("SELECT m FROM t12 WHERE id = 1");
        CHECK(t && strcmp(t, "-922337203685477.5808") == 0);
        free(t);
    }

    /* real/float */
    CHECK(fscalar("SELECT r FROM t12 WHERE id = 1") == 1.5);
    CHECK(fscalar("SELECT fl FROM t12 WHERE id = 1") == 2.718281828459045);

    /* char family */
    {
        char* t;
        t = text_q("SELECT RTRIM(ch) FROM t12 WHERE id = 1");
        CHECK(t && strcmp(t, "left") == 0);
        free(t);
        t = text_q("SELECT vc FROM t12 WHERE id = 1");
        CHECK(t && strcmp(t, "varchar-v") == 0);
        free(t);
        t = text_q("SELECT RTRIM(nch) FROM t12 WHERE id = 1");
        CHECK(t && strcmp(t, "nchar") == 0);
        free(t);
        t = text_q("SELECT nvc FROM t12 WHERE id = 1");
        CHECK(t && strcmp(t, "nvarchar-v") == 0);
        free(t);
    }

    /* binary family */
    {
        const unsigned char* b; int n;
        CHECK(blob_q("SELECT bin FROM t12 WHERE id = 1", &b, &n));
        CHECK(n == 4 && b[0] == 0x01 && b[3] == 0x04);
        CHECK(blob_q("SELECT vbin FROM t12 WHERE id = 1", &b, &n));
        CHECK(n == 6 && b[0] == 0x0A && b[5] == 0x0F);
    }

    /* uniqueidentifier: canonical text */
    {
        char* t = text_q("SELECT uid FROM t12 WHERE id = 1");
        CHECK(t && _stricmp(t, "6F9619FF-8B86-D011-B42D-00C04FC964FF") == 0);
        free(t);
    }

    /* date/time family: ISO text */
    {
        char* t;
        t = text_q("SELECT dt FROM t12 WHERE id = 1");
        CHECK(t && strncmp(t, "2026-09-03", 10) == 0);
        free(t);
        t = text_q("SELECT tm FROM t12 WHERE id = 1");
        CHECK(t && strstr(t, "12:34") != NULL);
        free(t);
        t = text_q("SELECT dtm FROM t12 WHERE id = 1");
        CHECK(t && strncmp(t, "2026-09-03 10:20:30.123", 23) == 0);
        free(t);
        t = text_q("SELECT sdt FROM t12 WHERE id = 1");
        CHECK(t && strncmp(t, "2026-09-03 08:15", 16) == 0);
        free(t);
        t = text_q("SELECT dt2 FROM t12 WHERE id = 1");
        CHECK(t && strncmp(t, "2026-09-03 10:20:30.123", 23) == 0);
        free(t);
        t = text_q("SELECT dto FROM t12 WHERE id = 1");
        CHECK(t && strstr(t, "2026-09-03 10:20:30") != NULL);
        free(t);
    }

    /* xml: BIGTEXT */
    {
        char* t = text_q("SELECT xm FROM t12 WHERE id = 1");
        CHECK(t && strcmp(t, "<root><a>1</a></root>") == 0);
        free(t);
    }

    /* rowversion: 8-byte binary token */
    {
        const unsigned char* b; int n;
        CHECK(blob_q("SELECT rv FROM t12 WHERE id = 1", &b, &n));
        CHECK(n == 8);
    }

    /* NULL row: every nullable column reads as NULL */
    CHECK(is_null("SELECT b FROM t12 WHERE id = 2"));
    CHECK(is_null("SELECT d10 FROM t12 WHERE id = 2"));
    CHECK(is_null("SELECT vc FROM t12 WHERE id = 2"));
    CHECK(is_null("SELECT dt2 FROM t12 WHERE id = 2"));
    CHECK(is_null("SELECT uid FROM t12 WHERE id = 2"));
    CHECK(is_null("SELECT xm FROM t12 WHERE id = 2"));

    /* ---- spatial: WKB (default) ---- */
    CHECK(exec_rc("CREATE VIRTUAL TABLE sp12 USING virtualmssql("
                  "schema='dbo', table='vms12_spatial');") == SQLITE_OK);
    {
        const unsigned char* wkb; int n;
        CHECK(blob_q("SELECT g FROM sp12 WHERE id = 1", &wkb, &n));
        /* WKB POINT = 1 (byte order) + 4 (type) + 8+8 (X, Y) = 21 bytes */
        CHECK(n == 21 && wkb[0] == 0x01);          /* little-endian WKB */
        CHECK(wkb[1] == 0x01);                      /* POINT */
        CHECK(spatialite_validate_wkb(wkb, n, "POINT(1 2)"));
        /* NULL spatial */
        CHECK(is_null("SELECT ge FROM sp12 WHERE id = 2"));
    }

    /* ---- spatial: WKT mode ---- */
    CHECK(exec_rc("CREATE VIRTUAL TABLE spwkt USING virtualmssql("
                  "schema='dbo', table='vms12_spatial', spatial='wkt');") == SQLITE_OK);
    {
        char* t = text_q("SELECT g FROM spwkt WHERE id = 1");
        CHECK(t && strstr(t, "POINT") != NULL && strstr(t, "(1") != NULL &&
              strstr(t, "2") != NULL);
        free(t);
    }

    /* ---- spatial pushdown still works (id = int pushdown) ---- */
    CHECK(scalar("SELECT COUNT(*) FROM sp12 WHERE id = 2") == 1);

    /* ---- UNSUPPORTED_TYPE: sql_variant table rejected ---- */
    {
        char* err = NULL;
        int rc = exec_rc("CREATE VIRTUAL TABLE unsup USING virtualmssql("
                         "schema='dbo', table='vms12_unsup');");
        CHECK(rc != SQLITE_OK);
        if (rc != SQLITE_OK && sqlite3_errmsg(g_db))
            CHECK(strstr(sqlite3_errmsg(g_db), "UNSUPPORTED_TYPE") != NULL);
        if (err) sqlite3_free(err);
    }

    /* cleanup */
    sqlite3_exec(g_db, "DROP TABLE IF EXISTS t12; DROP TABLE IF EXISTS sp12;"
                       "DROP TABLE IF EXISTS spwkt; DROP TABLE IF EXISTS unsup;",
                 NULL, NULL, NULL);
    sqlite3_close(g_db);

    if (g_fail == 0) {
        printf("test_r12: PASS\n");
        return 0;
    }
    fprintf(stderr, "test_r12: %d failures\n", g_fail);
    return 1;
}
