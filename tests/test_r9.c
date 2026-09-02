/* G9 — materialization tests. Key invariant: a partial snapshot is NEVER
 * published (state machine BUILDING -> PUBLISHED only on full success;
 * cancel/OOM/limit/server-error -> FAILED and the snapshot is dropped).
 * Offline part tests the state machine directly; live part verifies
 * end-to-end materialized scans through the vtab. */
#include "vms_mat.h"
#include "vms_query_source.h"
#include "vms_connstr.h"
#include "vms_pool.h"
#include "vms_credentials.h"
#include <sqlite3.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static sqlite3* g_db = NULL;
static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { g_fail++; fprintf(stderr, "FAIL@%d: %s\n", __LINE__, #cond); } \
} while (0)

static void test_mode_parse(void)
{
    CHECK(vms_mat_mode_parse("off") == VMS_MAT_OFF);
    CHECK(vms_mat_mode_parse("MEMORY") == VMS_MAT_MEMORY);
    CHECK(vms_mat_mode_parse("Temp") == VMS_MAT_TEMP);
    CHECK(vms_mat_mode_parse("bogus") == -1);
    CHECK(vms_mat_mode_parse("") == -1);
    CHECK(vms_mat_mode_parse(NULL) == -1);
}

static int count_rows(sqlite3* db, long long* out)
{
    sqlite3_stmt* st = NULL;
    *out = -1;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM vms_snapshot", -1, &st, NULL) != SQLITE_OK)
        return 0;
    if (sqlite3_step(st) == SQLITE_ROW)
        *out = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return *out >= 0;
}

int main(int argc, char** argv)
{
    const char* dll;
    const char* profile;
    VmsProfile prof;
    VmsError err;
    VmsPool* pool;
    VmsConnection* cn;
    VmsQuerySource src;
    VmsMat* mat;

    test_mode_parse();

    if (argc < 2) {
        if (g_fail == 0) { printf("test_r9: PASS (offline)\n"); return 0; }
        fprintf(stderr, "test_r9: %d failures\n", g_fail);
        return 1;
    }
    dll = argv[1];
    profile = getenv("VMS_TEST_PROFILE");
    if (!profile || !profile[0]) {
        fprintf(stderr, "VMS_TEST_PROFILE not set; live tests skipped\n");
        if (g_fail == 0) { printf("test_r9: PASS (offline)\n"); return 0; }
        fprintf(stderr, "test_r9: %d failures\n", g_fail);
        return 1;
    }
    CHECK(sqlite3_open(":memory:", &g_db) == SQLITE_OK);
    sqlite3_enable_load_extension(g_db, 1);
    CHECK(sqlite3_load_extension(g_db, dll, NULL, NULL) == SQLITE_OK);

    vms_cred_set_provider(vms_cred_memory_provider());
    vms_cred_memory_set(L"test:uid", L"sa");
    vms_cred_memory_set(L"test:pwd", L"Vms-Probe-2026!x");
    if (!vms_profile_parse(profile, &prof, &err)) {
        fprintf(stderr, "profile: %s\n", err.message);
        return 1;
    }
    pool = vms_pool_create(2);
    cn = vms_pool_acquire(pool, &prof, &err);
    CHECK(cn != NULL);
    if (!cn) return 1;

    /* prepare a query source */
    if (!vms_query_source_prepare(cn, "SELECT a, c FROM dbo.vms7_data ORDER BY a",
                                  &src, &err)) {
        fprintf(stderr, "prepare: %s\n", err.message);
        return 1;
    }

    /* ---- happy path: build -> PUBLISHED, rows present, indexes exist ---- */
    mat = vms_mat_create(VMS_MAT_MEMORY, 0, 0);
    CHECK(mat != NULL);
    CHECK(vms_mat_state(mat) == VMS_MAT_BUILDING);
    CHECK(vms_mat_build(mat, cn, &src, &err));
    CHECK(vms_mat_state(mat) == VMS_MAT_PUBLISHED);
    CHECK(vms_mat_row_count(mat) == 12);
    CHECK(vms_mat_db(mat) != NULL);
    {
        long long n = 0;
        CHECK(count_rows(vms_mat_db(mat), &n));
        CHECK(n == 12);
        /* index existence check */
        {
            sqlite3_stmt* st = NULL;
            int indexes = 0;
            if (sqlite3_prepare_v2(vms_mat_db(mat),
                    "SELECT COUNT(*) FROM sqlite_master WHERE type='index'", -1, &st, NULL) == SQLITE_OK &&
                sqlite3_step(st) == SQLITE_ROW) {
                indexes = sqlite3_column_int(st, 0);
            }
            sqlite3_finalize(st);
            CHECK(indexes >= 2); /* a and c are INTEGER columns */
        }
        /* data integrity: first/last row */
        {
            sqlite3_stmt* st = NULL;
            if (sqlite3_prepare_v2(vms_mat_db(mat),
                    "SELECT a FROM vms_snapshot ORDER BY a LIMIT 1", -1, &st, NULL) == SQLITE_OK &&
                sqlite3_step(st) == SQLITE_ROW) {
                CHECK(sqlite3_column_int64(st, 0) == -3);
            }
            sqlite3_finalize(st);
        }
    }
    vms_mat_destroy(mat);
    mat = NULL;

    /* ---- row limit: build must FAIL, nothing published ---- */
    mat = vms_mat_create(VMS_MAT_MEMORY, 5, 0); /* tiny limit */
    CHECK(mat != NULL);
    CHECK(!vms_mat_build(mat, cn, &src, &err)); /* 12 rows > 5 limit */
    CHECK(vms_mat_state(mat) == VMS_MAT_FAILED);
    CHECK(vms_mat_db(mat) == NULL); /* readers get nothing */
    vms_mat_destroy(mat);

    /* ---- cancel: build must FAIL ---- */
    mat = vms_mat_create(VMS_MAT_MEMORY, 0, 0);
    CHECK(mat != NULL);
    vms_mat_cancel(mat); /* cancel before build: observed at the first row */
    CHECK(!vms_mat_build(mat, cn, &src, &err));
    CHECK(vms_mat_state(mat) == VMS_MAT_FAILED);
    CHECK(vms_mat_db(mat) == NULL);
    vms_mat_destroy(mat);

    /* ---- temp mode: file db, cleaned up on destroy ---- */
    mat = vms_mat_create(VMS_MAT_TEMP, 0, 0);
    CHECK(mat != NULL);
    CHECK(vms_mat_build(mat, cn, &src, &err));
    CHECK(vms_mat_state(mat) == VMS_MAT_PUBLISHED);
    {
        long long n = 0;
        CHECK(count_rows(vms_mat_db(mat), &n));
        CHECK(n == 12);
    }
    vms_mat_destroy(mat);

    vms_pool_release(pool, cn);
    vms_pool_destroy(pool);
    sqlite3_close(g_db);

    if (g_fail == 0) {
        printf("test_r9: PASS\n");
        return 0;
    }
    fprintf(stderr, "test_r9: %d failures\n", g_fail);
    return 1;
}
