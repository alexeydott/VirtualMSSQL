/* G5 — metadata / stable key / identity token / trigger tests.
 * Offline part: identifier validation, token codec round-trips.
 * Live part (VMS_TEST_PROFILE): catalog reads, key selection incl. the
 * unsuitable-key rejection matrix, trigger listing. */
#include "vms_meta.h"
#include "vms_pool.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { g_fail++; fprintf(stderr, "FAIL@%d: %s\n", __LINE__, #cond); } \
} while (0)

static void test_identifiers(void)
{
    CHECK(vms_meta_ident_valid("dbo", 128));
    CHECK(vms_meta_ident_valid("t1_$#@x", 128));
    CHECK(!vms_meta_ident_valid("", 128));
    CHECK(!vms_meta_ident_valid(NULL, 128));
    CHECK(!vms_meta_ident_valid("a;b", 128));
    CHECK(!vms_meta_ident_valid("a'b", 128));
    CHECK(!vms_meta_ident_valid("DROP TABLE x", 128));
    CHECK(!vms_meta_ident_valid("verylongnamethatkeepsgoingandgoingandgoing_"
                                "andgoing_andgoing_andgoing_andgoing_andgoing_"
                                "andgoing_andgoing_andgoing_andgoing_andgoing_"
                                "andgoing_andgoing_andgoing_andgoing_andgoing_"
                                "andgoing_andgoing_andgoing_andgoing_x", 128));
    {
        char out[64];
        CHECK(vms_meta_quote_ident("tbl", out, sizeof(out)));
        CHECK(strcmp(out, "N'tbl'") == 0);
        CHECK(vms_meta_quote_ident("it's", out, sizeof(out)));
        CHECK(strcmp(out, "N'it''s'") == 0);
    }
}

static void test_identity_token(void)
{
    VmsValue parts[4];
    char tok[VMS_IDENTITY_TOKEN_MAX];
    VmsValue back[4];
    int n = 0;

    /* int + text + blob + null */
    memset(parts, 0, sizeof(parts));
    parts[0].type = VMS_VAL_INT64; parts[0].i = -1234567890123LL;
    parts[1].type = VMS_VAL_TEXT;
    parts[1].text = _strdup("привет|with pipe");
    parts[1].text_len = strlen(parts[1].text);
    parts[2].type = VMS_VAL_BLOB;
    parts[2].blob = (unsigned char*)"\x00\x01\xFE\xff";
    parts[2].blob_len = 4;
    parts[3].type = VMS_VAL_NULL;

    CHECK(vms_identity_encode(parts, 4, tok, sizeof(tok)));
    CHECK(strncmp(tok, "v1|", 3) == 0);
    CHECK(vms_identity_decode(tok, back, 4, &n));
    CHECK(n == 4);
    CHECK(back[0].type == VMS_VAL_INT64 && back[0].i == parts[0].i);
    CHECK(back[1].type == VMS_VAL_TEXT && back[1].text_len == parts[1].text_len);
    if (back[1].text) CHECK(memcmp(back[1].text, parts[1].text, parts[1].text_len) == 0);
    CHECK(back[2].type == VMS_VAL_BLOB && back[2].blob_len == 4);
    if (back[2].blob) CHECK(memcmp(back[2].blob, parts[2].blob, 4) == 0);
    CHECK(back[3].type == VMS_VAL_NULL);
    vms_identity_free(back, n);
    free(parts[1].text);

    /* truncation safety */
    {
        char smallbuf[8];
        CHECK(!vms_identity_encode(parts, 1, smallbuf, sizeof(smallbuf)));
    }
    /* garbage rejected */
    CHECK(!vms_identity_decode("v2|x", back, 4, &n));
    CHECK(!vms_identity_decode("hello", back, 4, &n));
}

/* ---- live tests ---- */

static int make_live_objects(VmsConnection* cn, VmsError* err)
{
    /* key-shape matrix:
     *  t_ok_int      : integer PK (best candidate)
     *  t_ok_guid     : GUID PK
     *  t_ok_comp     : composite 2-col PK
     *  t_ok_uniq     : no PK, suitable UNIQUE NOT NULL
     *  t_bad_null    : unique index over nullable column -> unsuitable
     *  t_bad_comp    : computed column in unique key -> unsuitable
     *  t_bad_filter  : filtered unique -> unsuitable
     *  t_pkplusuniq  : PK + extra unique: PK must win
     * plus a trigger table for trigger listing. */
    const wchar_t* ddl[] = {
        L"IF OBJECT_ID(N'dbo.vms5_bad_null') IS NOT NULL DROP TABLE dbo.vms5_bad_null;"
        L"IF OBJECT_ID(N'dbo.vms5_bad_comp') IS NOT NULL DROP TABLE dbo.vms5_bad_comp;"
        L"IF OBJECT_ID(N'dbo.vms5_bad_filter') IS NOT NULL DROP TABLE dbo.vms5_bad_filter;"
        L"IF OBJECT_ID(N'dbo.vms5_pkplusuniq') IS NOT NULL DROP TABLE dbo.vms5_pkplusuniq;"
        L"IF OBJECT_ID(N'dbo.vms5_ok_uniq') IS NOT NULL DROP TABLE dbo.vms5_ok_uniq;"
        L"IF OBJECT_ID(N'dbo.vms5_ok_comp') IS NOT NULL DROP TABLE dbo.vms5_ok_comp;"
        L"IF OBJECT_ID(N'dbo.vms5_ok_guid') IS NOT NULL DROP TABLE dbo.vms5_ok_guid;"
        L"IF OBJECT_ID(N'dbo.vms5_ok_int') IS NOT NULL DROP TABLE dbo.vms5_ok_int;"
        L"IF OBJECT_ID(N'dbo.vms5_trig') IS NOT NULL DROP TABLE dbo.vms5_trig;"
        L"IF OBJECT_ID(N'dbo.vms5_trig_log') IS NOT NULL DROP TABLE dbo.vms5_trig_log",
        L"CREATE TABLE dbo.vms5_ok_int(id int NOT NULL PRIMARY KEY, v nvarchar(50) NULL)",
        L"CREATE TABLE dbo.vms5_ok_guid(id uniqueidentifier NOT NULL PRIMARY KEY DEFAULT NEWID(), v int NULL)",
        L"CREATE TABLE dbo.vms5_ok_comp(a int NOT NULL, b nvarchar(20) NOT NULL, v int NULL, CONSTRAINT pk_vms5_comp PRIMARY KEY (a, b))",
        L"CREATE TABLE dbo.vms5_ok_uniq(id int NOT NULL, v int NULL)",
        L"CREATE UNIQUE INDEX ix_vms5_uniq ON dbo.vms5_ok_uniq(id)",
        L"CREATE TABLE dbo.vms5_bad_null(id int NULL, v int NULL)",
        L"CREATE UNIQUE INDEX ix_vms5_badnull ON dbo.vms5_bad_null(id)",
        L"CREATE TABLE dbo.vms5_bad_comp(id int NOT NULL, cc AS (id * 2) PERSISTED, v int NULL)",
        L"CREATE UNIQUE INDEX ix_vms5_badcomp ON dbo.vms5_bad_comp(cc)",
        L"CREATE TABLE dbo.vms5_bad_filter(id int NOT NULL, flag bit NOT NULL, v int NULL)",
        L"CREATE UNIQUE INDEX ix_vms5_badfilter ON dbo.vms5_bad_filter(id) WHERE flag = 1",
        L"CREATE TABLE dbo.vms5_pkplusuniq(id int NOT NULL PRIMARY KEY, code int NOT NULL, v int NULL)",
        L"CREATE UNIQUE INDEX ix_vms5_code ON dbo.vms5_pkplusuniq(code)",
        L"CREATE TABLE dbo.vms5_trig_log(id int NOT NULL PRIMARY KEY)",
        L"CREATE TABLE dbo.vms5_trig(id int NOT NULL PRIMARY KEY)",
        NULL
    };
    const wchar_t* trig =
        L"CREATE TRIGGER dbo.tr_vms5 ON dbo.vms5_trig AFTER INSERT AS"
        L" INSERT INTO dbo.vms5_trig_log(id) SELECT id FROM inserted";
    int i;
    for (i = 0; ddl[i]; i++) {
        VmsStatement* st = vms_stmt_exec_direct(cn, ddl[i], err);
        if (!st) return 0;
        vms_stmt_destroy(st);
    }
    {
        VmsStatement* st = vms_stmt_exec_direct(cn, trig, err);
        if (!st) return 0;
        vms_stmt_destroy(st);
    }
    return 1;
}

static void drop_live_objects(VmsConnection* cn, VmsError* err)
{
    VmsStatement* st = vms_stmt_exec_direct(cn,
        L"IF OBJECT_ID(N'dbo.tr_vms5', N'TR') IS NOT NULL DROP TRIGGER dbo.tr_vms5;"
        L"IF OBJECT_ID(N'dbo.vms5_bad_null') IS NOT NULL DROP TABLE dbo.vms5_bad_null;"
        L"IF OBJECT_ID(N'dbo.vms5_bad_comp') IS NOT NULL DROP TABLE dbo.vms5_bad_comp;"
        L"IF OBJECT_ID(N'dbo.vms5_bad_filter') IS NOT NULL DROP TABLE dbo.vms5_bad_filter;"
        L"IF OBJECT_ID(N'dbo.vms5_pkplusuniq') IS NOT NULL DROP TABLE dbo.vms5_pkplusuniq;"
        L"IF OBJECT_ID(N'dbo.vms5_ok_uniq') IS NOT NULL DROP TABLE dbo.vms5_ok_uniq;"
        L"IF OBJECT_ID(N'dbo.vms5_ok_comp') IS NOT NULL DROP TABLE dbo.vms5_ok_comp;"
        L"IF OBJECT_ID(N'dbo.vms5_ok_guid') IS NOT NULL DROP TABLE dbo.vms5_ok_guid;"
        L"IF OBJECT_ID(N'dbo.vms5_ok_int') IS NOT NULL DROP TABLE dbo.vms5_ok_int;"
        L"IF OBJECT_ID(N'dbo.vms5_trig') IS NOT NULL DROP TABLE dbo.vms5_trig;"
        L"IF OBJECT_ID(N'dbo.vms5_trig_log') IS NOT NULL DROP TABLE dbo.vms5_trig_log", err);
    if (st) vms_stmt_destroy(st);
}

static int key_is(VmsStableKey* k, int is_pk, int nparts)
{
    return k->is_primary_key == is_pk && k->part_count == nparts;
}

static void test_live(VmsConnection* cn)
{
    VmsError err;
    VmsStableKey key;
    VmsTableColumns cols;
    VmsTriggerList trig;

    CHECK(make_live_objects(cn, &err));
    if (g_fail) return;

    /* object kinds */
    CHECK(vms_meta_object_kind(cn, "dbo", "vms5_ok_int", &err) == VMS_OBJ_TABLE);
    CHECK(vms_meta_object_kind(cn, "dbo", "no_such_table_xyz", &err) == VMS_OBJ_ABSENT);

    /* columns + type registry */
    CHECK(vms_meta_columns(cn, "dbo", "vms5_ok_guid", &cols, &err));
    CHECK(cols.count == 2);
    CHECK(strcmp(cols.cols[0].name, "id") == 0 && cols.cols[0].vtype == VMS_CT_GUID);
    CHECK(strcmp(cols.cols[1].type_name, "int") == 0 && cols.cols[1].vtype == VMS_CT_INT64);
    CHECK(cols.cols[1].is_nullable == 1);

    CHECK(vms_meta_columns(cn, "dbo", "vms5_ok_comp", &cols, &err));
    CHECK(cols.count == 3);
    CHECK(cols.cols[1].vtype == VMS_CT_TEXT); /* nvarchar(20) */

    /* stable keys: good shapes */
    CHECK(vms_meta_stable_key(cn, "dbo", "vms5_ok_int", &key, &err));
    CHECK(key_is(&key, 1, 1));
    CHECK(strcmp(key.parts[0].name, "id") == 0 && key.parts[0].vtype == VMS_CT_INT64);

    CHECK(vms_meta_stable_key(cn, "dbo", "vms5_ok_guid", &key, &err));
    CHECK(key_is(&key, 1, 1));
    CHECK(key.parts[0].vtype == VMS_CT_GUID);

    CHECK(vms_meta_stable_key(cn, "dbo", "vms5_ok_comp", &key, &err));
    CHECK(key_is(&key, 1, 2));
    CHECK(strcmp(key.parts[0].name, "a") == 0 && strcmp(key.parts[1].name, "b") == 0);

    CHECK(vms_meta_stable_key(cn, "dbo", "vms5_ok_uniq", &key, &err));
    CHECK(key_is(&key, 0, 1)); /* unique fallback */

    /* PK preferred over plain unique */
    CHECK(vms_meta_stable_key(cn, "dbo", "vms5_pkplusuniq", &key, &err));
    CHECK(key_is(&key, 1, 1));
    CHECK(strcmp(key.parts[0].name, "id") == 0);

    /* unsuitable: nullable unique */
    CHECK(!vms_meta_stable_key(cn, "dbo", "vms5_bad_null", &key, &err));
    /* unsuitable: computed key column */
    CHECK(!vms_meta_stable_key(cn, "dbo", "vms5_bad_comp", &key, &err));
    /* unsuitable: filtered unique */
    CHECK(!vms_meta_stable_key(cn, "dbo", "vms5_bad_filter", &key, &err));

    /* triggers */
    CHECK(vms_meta_triggers(cn, "dbo", "vms5_trig", &trig, &err));
    CHECK(trig.count == 1);
    CHECK(strcmp(trig.names[0], "tr_vms5") == 0);
    CHECK(vms_meta_triggers(cn, "dbo", "vms5_ok_int", &trig, &err));
    CHECK(trig.count == 0);

    drop_live_objects(cn, &err);
}

int main(void)
{
    VmsProfile p;
    VmsError err;
    VmsPool* pool;
    VmsConnection* cn;

    test_identifiers();
    test_identity_token();

    if (!getenv("VMS_TEST_PROFILE")) {
        fprintf(stderr, "VMS_TEST_PROFILE not set; live metadata tests skipped\n");
        if (g_fail == 0) { printf("test_r5: PASS (offline)\n"); return 0; }
        fprintf(stderr, "test_r5: %d failures\n", g_fail);
        return 1;
    }

    vms_cred_set_provider(vms_cred_memory_provider());
    vms_cred_memory_set(L"r5:uid", L"sa");
    vms_cred_memory_set(L"r5:pwd", L"Vms-Probe-2026!x");
    vms_cred_memory_set(L"test:uid", L"sa");
    vms_cred_memory_set(L"test:pwd", L"Vms-Probe-2026!x");
    if (!vms_profile_parse(getenv("VMS_TEST_PROFILE"), &p, &err)) {
        fprintf(stderr, "profile parse failed: %s\n", err.message);
        return 1;
    }

    pool = vms_pool_create(1);
    cn = vms_pool_acquire(pool, &p, &err);
    CHECK(cn != NULL);
    if (cn) {
        test_live(cn);
        vms_pool_release(pool, cn);
    }
    vms_pool_destroy(pool);

    if (g_fail == 0) {
        printf("test_r5: PASS\n");
        return 0;
    }
    fprintf(stderr, "test_r5: %d failures\n", g_fail);
    return 1;
}
