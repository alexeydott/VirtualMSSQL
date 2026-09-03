/* seed script: create the R6 test fixtures on the live server */
#include "vms_client.h"
#include "vms_connstr.h"
#include "vms_credentials.h"
#include "vms_pool.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    VmsError err;
    VmsProfile profile;
    VmsPool* pool;
    VmsConnection* cn;
    VmsStatement* st;
    const char* spec = getenv("VMS_TEST_PROFILE");
    static const wchar_t* ddl[] = {
        L"IF OBJECT_ID(N'dbo.vms10_dml') IS NOT NULL DROP TABLE dbo.vms10_dml;"
        L"IF OBJECT_ID(N'dbo.vms10_conf') IS NOT NULL DROP TABLE dbo.vms10_conf;"
        L"IF OBJECT_ID(N'dbo.vms10_trg') IS NOT NULL DROP TABLE dbo.vms10_trg;"
        L"IF OBJECT_ID(N'dbo.vms10_trg_log') IS NOT NULL DROP TABLE dbo.vms10_trg_log;"
        L"IF OBJECT_ID(N'dbo.vms10_comp') IS NOT NULL DROP TABLE dbo.vms10_comp;"
        L"IF OBJECT_ID(N'dbo.vms10_rv') IS NOT NULL DROP TABLE dbo.vms10_rv;"
        L"IF OBJECT_ID(N'dbo.vms10_io_trg') IS NOT NULL DROP VIEW dbo.vms10_io_trg;"
        L"IF OBJECT_ID(N'dbo.vms7_data') IS NOT NULL DROP TABLE dbo.vms7_data;"
        L"IF OBJECT_ID(N'dbo.vms6_view') IS NOT NULL DROP VIEW dbo.vms6_view;"
        L"IF OBJECT_ID(N'dbo.vms6_t_empty') IS NOT NULL DROP TABLE dbo.vms6_t_empty;"
        L"IF OBJECT_ID(N'dbo.vms6_t_big') IS NOT NULL DROP TABLE dbo.vms6_t_big;"
        L"IF OBJECT_ID(N'dbo.vms6_t_lob') IS NOT NULL DROP TABLE dbo.vms6_t_lob;"
        L"IF OBJECT_ID(N'dbo.vms6_t_all') IS NOT NULL DROP TABLE dbo.vms6_t_all;"
        L"IF OBJECT_ID(N'dbo.vms6_t_int') IS NOT NULL DROP TABLE dbo.vms6_t_int",
        /* R10: DML fixtures with every key shape */
        L"CREATE TABLE dbo.vms10_dml("
        L" id int NOT NULL PRIMARY KEY,"
        L" name nvarchar(50) NOT NULL,"
        L" val int NULL,"
        L" txt nvarchar(100) NULL)",
        L"CREATE TABLE dbo.vms10_conf("
        L" id int NOT NULL PRIMARY KEY,"
        L" rv rowversion,"
        L" val int NULL)",
        L"CREATE TABLE dbo.vms10_trg("
        L" id int NOT NULL PRIMARY KEY,"
        L" v int NOT NULL)",
        L"CREATE TABLE dbo.vms10_trg_log(id int NOT NULL PRIMARY KEY, v int NOT NULL)",
        L"CREATE TABLE dbo.vms10_comp("
        L" ka nvarchar(20) NOT NULL,"
        L" kb int NOT NULL,"
        L" val int NULL,"
        L" CONSTRAINT pk_vms10_comp PRIMARY KEY (ka, kb))",
        L"CREATE TABLE dbo.vms10_rv("
        L" id uniqueidentifier NOT NULL PRIMARY KEY DEFAULT NEWID(),"
        L" v int NULL)",
        L"CREATE VIEW dbo.vms10_io_trg AS SELECT id, v FROM dbo.vms10_trg",
        L"INSERT INTO dbo.vms10_dml(id, name, val, txt) VALUES"
        L"(1, N'one', 10, N'text-one'), (2, N'two', 20, N'text-two'),"
        L"(3, N'three', 30, NULL)",
        L"INSERT INTO dbo.vms10_conf(id, val) VALUES(1, 100), (2, 200)",
        L"INSERT INTO dbo.vms10_comp(ka, kb, val) VALUES(N'k1', 1, 11), (N'k2', 2, 22)",
        L"INSERT INTO dbo.vms10_rv(v) VALUES(5), (6)",
        L"INSERT INTO dbo.vms10_trg(id, v) VALUES(1, 10), (2, 20)",
        L"CREATE TRIGGER dbo.tr_vms10_upd ON dbo.vms10_trg AFTER UPDATE AS"
        L" UPDATE t SET v = t.v + 1 FROM dbo.vms10_trg t"
        L" JOIN inserted i ON i.id = t.id;"
        L" INSERT INTO dbo.vms10_trg_log(id, v) SELECT i.id, i.v + 1"
        L" FROM inserted i",
        L"CREATE TABLE dbo.vms7_data(a int NOT NULL PRIMARY KEY, b nvarchar(50) NOT NULL, c int NULL)",
        L"INSERT INTO dbo.vms7_data(a, b, c) VALUES"
        L"(-3, N'alpha', NULL), (1, N'bravo', 10), (2, N'charlie', NULL),"
        L"(3, N'delta', 30), (5, N'echo', NULL), (7, N'foxtrot', 70),"
        L"(8, N'golf', 80), (11, N'hotel', NULL), (12, N'india', 120),"
        L"(15, N'juliet', 150), (16, N'kilo', NULL), (20, N'lima', 200)",
        L"CREATE TABLE dbo.vms6_t_int(id int NOT NULL PRIMARY KEY, v int NULL)",
        L"INSERT INTO dbo.vms6_t_int VALUES(1, 100), (2, 200), (3, 300)",
        L"CREATE TABLE dbo.vms6_t_all("
        L" i bigint NOT NULL PRIMARY KEY,"
        L" f float NULL,"
        L" t nvarchar(200) NULL,"
        L" nul int NULL,"
        L" d datetime2(3) NULL,"
        L" g uniqueidentifier NULL)",
        L"INSERT INTO dbo.vms6_t_all(i, f, t, nul, d, g) VALUES"
        L"(42, 2.5, N'привет 🚀 мира', NULL, '2026-09-02T10:20:30.123',"
        L" '6F9619FF-8B86-D011-B42D-00C04FC964FF')",
        L"CREATE TABLE dbo.vms6_t_lob("
        L" id int NOT NULL PRIMARY KEY,"
        L" bigt nvarchar(max) NULL,"
        L" bigb varbinary(max) NULL)",
        L"CREATE TABLE dbo.vms6_t_empty(id int NOT NULL PRIMARY KEY)",
        L"CREATE TABLE dbo.vms6_t_big(id int NOT NULL PRIMARY KEY, i bigint NOT NULL)",
        L"CREATE VIEW dbo.vms6_view AS SELECT id, v FROM dbo.vms6_t_int",
        NULL
    };
    wchar_t bigins[2200];
    int i;

    (void)bigins;
    if (!spec) { fprintf(stderr, "VMS_TEST_PROFILE not set\n"); return 1; }
    vms_cred_set_provider(vms_cred_memory_provider());
    vms_cred_memory_set(L"test:uid", L"sa");
    vms_cred_memory_set(L"test:pwd", L"Vms-Probe-2026!x");
    if (!vms_profile_parse(spec, &profile, &err)) {
        fprintf(stderr, "profile: %s\n", err.message);
        return 1;
    }    pool = vms_pool_create(1);
    cn = vms_pool_acquire(pool, &profile, &err);
    if (!cn) { fprintf(stderr, "conn: %s\n", err.message); return 1; }

    /* lob row: 200k nvarchar chars via REPLICATE; binary via replicate+cast */
    for (i = 0; ddl[i]; i++) {
        st = vms_stmt_exec_direct(cn, ddl[i], &err);
        if (!st) { fprintf(stderr, "ddl[%d]: %s\n", i, err.message); return 1; }
        vms_stmt_destroy(st);
    }
    {
        const wchar_t* extra[] = {
            L"INSERT INTO dbo.vms6_t_lob(id, bigt, bigb) VALUES"
            L"(1, REPLICATE(N'0123456789', 20000),"
            L" CAST(0xAB AS varbinary(max)) + CAST(0xAB AS varbinary(max)))",
            L"UPDATE dbo.vms6_t_lob SET bigt = REPLICATE(bigt, 10) WHERE id = 1",
            L"UPDATE dbo.vms6_t_lob SET bigb = CONVERT(varbinary(max), REPLICATE(CAST(0xAB AS varbinary(max)), 50000)) WHERE id = 1",
            L"INSERT INTO dbo.vms6_t_big(id, i) SELECT TOP (100000) "
            L"ROW_NUMBER() OVER (ORDER BY (SELECT NULL)),"
            L" ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) "
            L"FROM sys.all_objects a CROSS JOIN sys.all_objects b",
            NULL
        };
        for (i = 0; extra[i]; i++) {
            st = vms_stmt_exec_direct(cn, extra[i], &err);
            if (!st) { fprintf(stderr, "extra[%d]: %s\n", i, err.message); return 1; }
            vms_stmt_destroy(st);
        }
    }
    vms_pool_release(pool, cn);
    vms_pool_destroy(pool);
    printf("seed done\n");
    return 0;
}
