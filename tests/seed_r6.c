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
        /* R12: mandatory type matrix + spatial fixtures */
        L"IF OBJECT_ID(N'dbo.vms12_types') IS NOT NULL DROP TABLE dbo.vms12_types;"
        L"IF OBJECT_ID(N'dbo.vms12_spatial') IS NOT NULL DROP TABLE dbo.vms12_spatial;"
        L"IF OBJECT_ID(N'dbo.vms12_unsup') IS NOT NULL DROP TABLE dbo.vms12_unsup",
        L"CREATE TABLE dbo.vms12_types("
        L" id int NOT NULL PRIMARY KEY,"
        L" b bit NULL, ti tinyint NULL, si smallint NULL, bi bigint NULL,"
        L" d10 decimal(10,2) NULL, m money NULL,"
        L" r real NULL, fl float NULL,"
        L" ch char(10) NULL, vc varchar(50) NULL, nch nchar(10) NULL, nvc nvarchar(50) NULL,"
        L" bin binary(4) NULL, vbin varbinary(16) NULL,"
        L" uid uniqueidentifier NULL,"
        L" dt date NULL, tm time NULL, dtm datetime NULL, sdt smalldatetime NULL,"
        L" dt2 datetime2(3) NULL, dto datetimeoffset(3) NULL,"
        L" xm xml NULL, rv rowversion)",
        L"INSERT INTO dbo.vms12_types(id, b, ti, si, bi, d10, m, r, fl, ch, vc, nch, nvc,"
        L" bin, vbin, uid, dt, tm, dtm, sdt, dt2, dto, xm)"
        L" VALUES(1, 1, 255, -32768, 9223372036854775807,"
        L" 12345678.90, -922337203685477.5808, 1.5, 2.718281828459045,"
        L" N'left', N'varchar-v', N'nchar', N'nvarchar-v',"
        L" 0x01020304, 0x0A0B0C0D0E0F,"
        L" '6F9619FF-8B86-D011-B42D-00C04FC964FF',"
        L" '2026-09-03', '12:34:56.789', '2026-09-03T10:20:30.123',"
        L" '2026-09-03T08:15:00', '2026-09-03T10:20:30.123+02:00',"
        L" '2026-09-03T10:20:30.1234567+02:00', N'<root><a>1</a></root>')",
        L"INSERT INTO dbo.vms12_types(id) VALUES(2)",
        L"CREATE TABLE dbo.vms12_spatial("
        L" id int NOT NULL PRIMARY KEY,"
        L" g geometry NULL, ge geography NULL)",
        L"INSERT INTO dbo.vms12_spatial(id, g, ge) VALUES"
        L"(1, geometry::STGeomFromText('POINT(1 2)', 0),"
        L"   geography::STGeomFromText('POINT(30 40)', 4326)),"
        L"(2, geometry::STGeomFromText('LINESTRING(0 0, 10 10)', 0), NULL),"
        L"(3, NULL, NULL)",
        L"CREATE TABLE dbo.vms12_unsup(id int NOT NULL PRIMARY KEY, sv sql_variant NULL)",
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
        /* R15 performance fixtures */
        L"IF OBJECT_ID(N'dbo.vms15_wide') IS NOT NULL DROP TABLE dbo.vms15_wide;"
        L"IF OBJECT_ID(N'dbo.vms15_lob') IS NOT NULL DROP TABLE dbo.vms15_lob",
        L"CREATE TABLE dbo.vms15_wide("
        L" id int NOT NULL PRIMARY KEY,"
        L" c1 nvarchar(80), c2 nvarchar(80), c3 nvarchar(80), c4 nvarchar(80),"
        L" c5 nvarchar(80), c6 nvarchar(80), c7 nvarchar(80), c8 nvarchar(80),"
        L" n1 int, n2 int, n3 int, n4 int, f1 float, f2 float, g1 uniqueidentifier)",
        L"INSERT INTO dbo.vms15_wide"
        L" (id, c1, c2, c3, c4, c5, c6, c7, c8, n1, n2, n3, n4, f1, f2, g1)"
        L" SELECT TOP (5000)"
        L" ROW_NUMBER() OVER (ORDER BY (SELECT NULL)),"
        L" REPLICATE(N'w', 80), REPLICATE(N'x', 80), REPLICATE(N'y', 80), REPLICATE(N'z', 80),"
        L" REPLICATE(N'a', 80), REPLICATE(N'b', 80), REPLICATE(N'c', 80), REPLICATE(N'd', 80),"
        L" 1, 2, 3, 4, 0.5, 1.5, NEWID()"
        L" FROM sys.all_objects a CROSS JOIN sys.all_objects b",
        NULL
    };
    wchar_t bigins[2200];
    int i;

    (void)bigins;
    if (!spec) { fprintf(stderr, "VMS_TEST_PROFILE not set\n"); return 1; }
    vms_cred_set_provider(vms_cred_memory_provider());
    /* credentials: defaults match the localhost test server; override with
     * VMS_BENCH_UID / VMS_BENCH_PWD for remote seed targets */
    {
        const char* uid = getenv("VMS_BENCH_UID");
        const char* pwd = getenv("VMS_BENCH_PWD");
        wchar_t wuid[128], wpwd[256];
        MultiByteToWideChar(CP_UTF8, 0, uid ? uid : "sa", -1, wuid, 128);
        MultiByteToWideChar(CP_UTF8, 0, pwd ? pwd : "Vms-Probe-2026!x", -1, wpwd, 256);
        vms_cred_memory_set(L"test:uid", wuid);
        vms_cred_memory_set(L"test:pwd", wpwd);
    }
    if (!vms_profile_parse(spec, &profile, &err)) {
        fprintf(stderr, "profile: %s\n", err.message);
        return 1;
    }    pool = vms_pool_create(1);
    cn = vms_pool_acquire(pool, &profile, &err);
    if (!cn) { fprintf(stderr, "conn: %s\n", err.message); return 1; }

    /* lob row: 200k nvarchar chars via REPLICATE; binary via replicate+cast */
    for (i = 0; ddl[i]; i++) {
        int attempt;
        for (attempt = 0; attempt < 3; attempt++) {
            st = vms_stmt_exec_direct(cn, ddl[i], &err);
            if (st) break;
            /* pooled ctest sessions can briefly hold metadata locks on
             * DROP TABLE; retry a few times before giving up */
            fprintf(stderr, "ddl[%d] attempt %d: %s\n", i, attempt + 1,
                    err.message);
            Sleep(700);
        }
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
            /* R15: 1M-row narrow table (vms15_million; idempotent reseed) */
            L"IF OBJECT_ID(N'dbo.vms15_million') IS NOT NULL DROP TABLE dbo.vms15_million",
            L"CREATE TABLE dbo.vms15_million(id bigint NOT NULL PRIMARY KEY, i bigint NOT NULL)",
            L"INSERT INTO dbo.vms15_million(id, i) SELECT TOP (1000000)"
            L" ROW_NUMBER() OVER (ORDER BY (SELECT NULL)),"
            L" ROW_NUMBER() OVER (ORDER BY (SELECT NULL))"
            L" FROM sys.all_objects a CROSS JOIN sys.all_objects b"
            L" CROSS JOIN sys.all_objects c",
            /* R15: LOB table with 1MB/16MB/64MB rows */
            L"IF OBJECT_ID(N'dbo.vms15_lob') IS NOT NULL DROP TABLE dbo.vms15_lob",
            L"CREATE TABLE dbo.vms15_lob(id int NOT NULL PRIMARY KEY, sz int NOT NULL, bigt nvarchar(max) NULL, bigb varbinary(max) NULL)",
            L"INSERT INTO dbo.vms15_lob(id, sz, bigt) VALUES"
            L" (1, 1, REPLICATE(CAST(N'A' AS nvarchar(max)), 512*1024)),"
            L" (2, 16, REPLICATE(CAST(N'B' AS nvarchar(max)), 8*1024*1024)),"
            L" (3, 64, REPLICATE(CAST(N'C' AS nvarchar(max)), 32*1024*1024))",
            NULL
        };
        for (i = 0; extra[i]; i++) {
            int attempt;
            for (attempt = 0; attempt < 3; attempt++) {
                st = vms_stmt_exec_direct(cn, extra[i], &err);
                if (st) break;
                fprintf(stderr, "extra[%d] attempt %d: %s\n", i, attempt + 1,
                        err.message);
                Sleep(700);
            }
            if (!st) { fprintf(stderr, "extra[%d]: %s\n", i, err.message); return 1; }
            vms_stmt_destroy(st);
        }
    }
    vms_pool_release(pool, cn);
    vms_pool_destroy(pool);
    printf("seed done\n");
    return 0;
}
