/* R0.6 — transactions: AUTOBEGINTXN, savepoints, XACT_STATE, commit fault, OUTPUT INTO */
#include "probe.h"
#include <string.h>
#include <stdlib.h>

#ifndef SQL_COPT_SS_AUTOBEGINTXN
/* real value per msodbcsql.h (Driver 18): SQL_COPT_SS_BASE_ADD(1400) + 2 */
#define SQL_COPT_SS_AUTOBEGINTXN 1402
#endif
#ifndef SQL_AUTOBEGINTXN_ON
#define SQL_AUTOBEGINTXN_ON 1
#endif
#ifndef SQL_AUTOBEGINTXN_OFF
#define SQL_AUTOBEGINTXN_OFF 0
#endif

static bool tx_exec(ProbeCtx* ctx, HDBC dbc, const wchar_t* sql, ProbeDiag* diag)
{
    SQLHSTMT st = SQL_NULL_HSTMT;
    SQLRETURN r;
    diag_reset(diag);
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &st))) return false;
    r = SQLExecDirectW(st, sql, SQL_NTS);
    if (r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO || r == SQL_NO_DATA) {
        while (SQLMoreResults(st) == SQL_SUCCESS_WITH_INFO) {}
        SQLFreeHandle(SQL_HANDLE_STMT, st);
        return true;
    }
    diag_capture(ctx, SQL_HANDLE_STMT, st, diag);
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    return false;
}

static bool tx_query_i64(ProbeCtx* ctx, HDBC dbc, const wchar_t* sql, SQLBIGINT* out, ProbeDiag* diag)
{
    SQLHSTMT st = SQL_NULL_HSTMT;
    SQLLEN ind = 0;
    diag_reset(diag);
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &st))) return false;
    if (SQL_SUCCEEDED(SQLExecDirectW(st, sql, SQL_NTS)) && SQL_SUCCEEDED(SQLFetch(st)) &&
        SQL_SUCCEEDED(SQLGetData(st, 1, SQL_C_SBIGINT, out, 0, &ind))) {
        SQLFreeHandle(SQL_HANDLE_STMT, st);
        return true;
    }
    diag_capture(ctx, SQL_HANDLE_STMT, st, diag);
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    return false;
}

static HDBC tx_connect(ProbeCtx* ctx, ProbeDiag* diag, int autobegin_off)
{
    HDBC dbc = SQL_NULL_HDBC;
    wchar_t* wcs;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, ctx->env, &dbc))) return SQL_NULL_HDBC;
    if (autobegin_off) {
        if (!SQL_SUCCEEDED(SQLSetConnectAttr(dbc, SQL_COPT_SS_AUTOBEGINTXN,
                                             (SQLPOINTER)SQL_AUTOBEGINTXN_OFF, SQL_IS_INTEGER))) {
            diag_capture(ctx, SQL_HANDLE_DBC, dbc, diag);
            SQLFreeHandle(SQL_HANDLE_DBC, dbc);
            return SQL_NULL_HDBC;
        }
    }
    if (!SQL_SUCCEEDED(SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT,
                                         (SQLPOINTER)SQL_AUTOCOMMIT_OFF, SQL_IS_INTEGER))) {
        diag_capture(ctx, SQL_HANDLE_DBC, dbc, diag);
        SQLFreeHandle(SQL_HANDLE_DBC, dbc);
        return SQL_NULL_HDBC;
    }
    wcs = connstr_build(ctx, ctx->cfg.server, "sql", "trust_server_certificate", NULL);
    if (!conn_connect(ctx, dbc, wcs, diag)) {
        free(wcs);
        SQLFreeHandle(SQL_HANDLE_DBC, dbc);
        return SQL_NULL_HDBC;
    }
    free(wcs);
    return dbc;
}

/* case: AUTOBEGINTXN OFF -> lazy tx start.
 * NOTE: @@TRANCOUNT cannot be observed "before the first statement" because the
 * observing query itself is a statement; measure AFTER a plain SELECT instead. */
static int case_autobegin(ProbeCtx* ctx)
{
    int idx = ctx->case_count;
    ProbeDiag diag;
    HDBC dbc;
    SQLBIGINT trancount = -1;

    case_add(ctx, "tx", "autobegin_off_lazy", PROBE_SKIP, NULL);
    dbc = tx_connect(ctx, &diag, 1);
    if (dbc == SQL_NULL_HDBC) {
        ctx->cases[idx].diag = diag;
        case_set_status(ctx, idx, PROBE_FAIL, "connect failed");
        return 1;
    }
    /* verified: no server-side tx right after connect; the FIRST statement
     * (even SELECT) opens it. @@TRANCOUNT after a plain SELECT must be 1. */
    if (!tx_exec(ctx, dbc, L"SELECT 1", &diag) ||
        !tx_query_i64(ctx, dbc, L"SELECT @@TRANCOUNT", &trancount, &diag)) {
        ctx->cases[idx].diag = diag;
        case_set_status(ctx, idx, PROBE_FAIL, "query @@TRANCOUNT failed");
        SQLDisconnect(dbc); SQLFreeHandle(SQL_HANDLE_DBC, dbc);
        return 1;
    }
    if (trancount != 1) {
        case_set_status(ctx, idx, PROBE_FAIL,
            "@@TRANCOUNT=%lld after plain SELECT (expected 1: lazy begin at first stmt)", trancount);
        SQLDisconnect(dbc); SQLFreeHandle(SQL_HANDLE_DBC, dbc);
        return 1;
    }
    /* ROLLBACK closes the tx; the next observation statement re-opens it,
     * so both 0 (fresh session had no tx) and 1 (re-opened by this query)
     * are consistent with lazy-begin semantics. */
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK);
    if (!tx_query_i64(ctx, dbc, L"SELECT @@TRANCOUNT", &trancount, &diag)) {
        ctx->cases[idx].diag = diag;
        case_set_status(ctx, idx, PROBE_FAIL, "post-rollback @@TRANCOUNT query failed");
        SQLDisconnect(dbc); SQLFreeHandle(SQL_HANDLE_DBC, dbc);
        return 1;
    }
    if (trancount == 0 || trancount == 1) {
        case_set_status(ctx, idx, PROBE_PASS,
            "lazy begin at first stmt confirmed; ROLLBACK closed tx (post-check @@TRANCOUNT=%lld)",
            trancount);
    } else {
        case_set_status(ctx, idx, PROBE_FAIL, "@@TRANCOUNT=%lld after ROLLBACK (unexpected)", trancount);
    }
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK);
    SQLDisconnect(dbc); SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    return 1;
}

/* case: savepoint semantics incl. before first business statement (primer) */
static int case_savepoints(ProbeCtx* ctx)
{
    int idx = ctx->case_count;
    ProbeDiag diag;
    HDBC dbc;
    SQLBIGINT v = -1;
    int ok = 0;
    int primer_ok = 0;

    case_add(ctx, "tx", "savepoints", PROBE_SKIP, NULL);
    dbc = tx_connect(ctx, &diag, 1);
    if (dbc == SQL_NULL_HDBC) {
        ctx->cases[idx].diag = diag;
        case_set_status(ctx, idx, PROBE_FAIL, "connect failed");
        return 1;
    }
    tx_exec(ctx, dbc, L"IF OBJECT_ID(N'dbo.vms_probe_tx') IS NOT NULL DROP TABLE dbo.vms_probe_tx;"
                      L"CREATE TABLE dbo.vms_probe_tx(id int NOT NULL PRIMARY KEY, v int NOT NULL)", &diag);
    /* SQLEndTran commit schema */
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);

    /* PRIMER TEST: savepoint before any business statement in the tx */
    if (tx_exec(ctx, dbc, L"SAVE TRANSACTION vms_sp_1", &diag)) {
        primer_ok = 1;
        tx_exec(ctx, dbc, L"INSERT INTO dbo.vms_probe_tx(id,v) VALUES(1,100)", &diag);
        tx_exec(ctx, dbc, L"ROLLBACK TRANSACTION vms_sp_1", &diag);
    } else {
        /* try classic primer: begin a statement first */
        tx_exec(ctx, dbc, L"SELECT 1", &diag);
        if (tx_exec(ctx, dbc, L"SAVE TRANSACTION vms_sp_1", &diag)) {
            primer_ok = 2;
            tx_exec(ctx, dbc, L"INSERT INTO dbo.vms_probe_tx(id,v) VALUES(1,100)", &diag);
            tx_exec(ctx, dbc, L"ROLLBACK TRANSACTION vms_sp_1", &diag);
        }
    }
    /* normal savepoint flow */
    tx_exec(ctx, dbc, L"INSERT INTO dbo.vms_probe_tx(id,v) VALUES(2,200)", &diag);
    if (tx_exec(ctx, dbc, L"SAVE TRANSACTION vms_sp_2", &diag) &&
        tx_exec(ctx, dbc, L"INSERT INTO dbo.vms_probe_tx(id,v) VALUES(3,300)", &diag) &&
        tx_exec(ctx, dbc, L"ROLLBACK TRANSACTION vms_sp_2", &diag)) {
        ok = 1;
    }
    if (tx_query_i64(ctx, dbc, L"SELECT COUNT(*) FROM dbo.vms_probe_tx", &v, &diag) && ok) {
        if (v == 1) {
            case_set_status(ctx, idx, PROBE_PASS,
                "savepoints OK; primer before-first-statement %s",
                primer_ok == 1 ? "SUPPORTED" : (primer_ok == 2 ? "needs statement primer" : "unresolved"));
        } else {
            case_set_status(ctx, idx, PROBE_FAIL, "row count %lld after savepoint rollback (expected 1)", v);
        }
    } else {
        ctx->cases[idx].diag = diag;
        case_set_status(ctx, idx, PROBE_FAIL, "savepoint flow failed");
    }
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK);
    tx_exec(ctx, dbc, L"IF OBJECT_ID(N'dbo.vms_probe_tx') IS NOT NULL DROP TABLE dbo.vms_probe_tx", &diag);
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    SQLDisconnect(dbc); SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    return 1;
}

/* case: XACT_STATE after error; XACT_STATE()=-1 -> only full rollback */
static int case_xact_state(ProbeCtx* ctx)
{
    int idx = ctx->case_count;
    ProbeDiag diag;
    HDBC dbc;
    SQLBIGINT xst = -99;
    int pass = 0;

    case_add(ctx, "tx", "xact_state", PROBE_SKIP, NULL);
    dbc = tx_connect(ctx, &diag, 1);
    if (dbc == SQL_NULL_HDBC) {
        ctx->cases[idx].diag = diag;
        case_set_status(ctx, idx, PROBE_FAIL, "connect failed");
        return 1;
    }
    tx_exec(ctx, dbc, L"IF OBJECT_ID(N'dbo.vms_probe_tx') IS NOT NULL DROP TABLE dbo.vms_probe_tx;"
                      L"CREATE TABLE dbo.vms_probe_tx(id int NOT NULL PRIMARY KEY, v int NOT NULL)", &diag);
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);

    tx_exec(ctx, dbc, L"INSERT INTO dbo.vms_probe_tx(id,v) VALUES(1,1)", &diag);
    /* induce uncommittable error: PK violation inside explicit tx leaves tx committable;
       use trigger-free doom: RAISERROR with XACT_ABORT */
    if (!tx_exec(ctx, dbc,
        L"SET XACT_ABORT ON; INSERT INTO dbo.vms_probe_tx(id,v) VALUES(1,2)", &diag)) {
        /* expected: statement failed; transaction may be uncommittable */
        if (tx_query_i64(ctx, dbc, L"SELECT XACT_STATE()", &xst, &diag)) {
            if (xst == 1) {
                case_set_status(ctx, idx, PROBE_PASS, "XACT_STATE()=1 (committable) after caught error");
                SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK);
                pass = 1;
            } else if (xst == -1) {
                case_set_status(ctx, idx, PROBE_PASS, "XACT_STATE()=-1 confirmed; only full rollback allowed");
                SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK);
                pass = 1;
            } else if (xst == 0) {
                case_set_status(ctx, idx, PROBE_PASS, "XACT_STATE()=0 (tx auto-terminated by XACT_ABORT)");
                pass = 1;
            } else {
                case_set_status(ctx, idx, PROBE_FAIL, "XACT_STATE()=%lld unexpected", xst);
            }
        } else {
            ctx->cases[idx].diag = diag;
            case_set_status(ctx, idx, PROBE_FAIL, "cannot query XACT_STATE()");
        }
    } else {
        case_set_status(ctx, idx, PROBE_FAIL, "duplicate PK insert unexpectedly succeeded");
    }
    tx_exec(ctx, dbc, L"IF OBJECT_ID(N'dbo.vms_probe_tx') IS NOT NULL DROP TABLE dbo.vms_probe_tx", &diag);
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    SQLDisconnect(dbc); SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    return 1;
}

/* case: OUTPUT INTO pre-trigger state + trigger behavior */
static int case_output_into(ProbeCtx* ctx)
{
    int idx = ctx->case_count;
    ProbeDiag diag;
    HDBC dbc;
    SQLBIGINT v = -1;

    case_add(ctx, "tx", "output_into_trigger", PROBE_SKIP, NULL);
    dbc = tx_connect(ctx, &diag, 1);
    if (dbc == SQL_NULL_HDBC) {
        ctx->cases[idx].diag = diag;
        case_set_status(ctx, idx, PROBE_FAIL, "connect failed");
        return 1;
    }
    /* CREATE TRIGGER must be the first statement of its own batch */
    if (!tx_exec(ctx, dbc,
        L"IF OBJECT_ID(N'dbo.vms_probe_tx') IS NOT NULL DROP TABLE dbo.vms_probe_tx;"
        L"CREATE TABLE dbo.vms_probe_tx(id int NOT NULL PRIMARY KEY, v int NOT NULL);"
        L"IF OBJECT_ID(N'dbo.vms_probe_log') IS NOT NULL DROP TABLE dbo.vms_probe_log;"
        L"CREATE TABLE dbo.vms_probe_log(id int NOT NULL PRIMARY KEY);", &diag) ||
        !tx_exec(ctx, dbc,
        L"CREATE TRIGGER dbo.tr_vms_probe_tx ON dbo.vms_probe_tx AFTER INSERT AS"
        L" INSERT INTO dbo.vms_probe_log(id) SELECT id FROM inserted;", &diag)) {
        ctx->cases[idx].diag = diag;
        case_set_status(ctx, idx, PROBE_FAIL, "DDL failed (see log)");
        SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK);
        SQLDisconnect(dbc); SQLFreeHandle(SQL_HANDLE_DBC, dbc);
        return 1;
    }
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);

    /* OUTPUT INTO with AFTER trigger must succeed and return pre-trigger values */
    if (tx_exec(ctx, dbc,
        L"DECLARE @t TABLE(id int);"
        L"INSERT INTO dbo.vms_probe_tx(id,v) OUTPUT inserted.id INTO @t VALUES(10,10);"
        L"INSERT INTO dbo.vms_probe_log(id) SELECT id FROM @t WHERE id NOT IN (SELECT id FROM dbo.vms_probe_log);",
        &diag)) {
        if (tx_query_i64(ctx, dbc, L"SELECT COUNT(*) FROM dbo.vms_probe_tx WHERE id=10", &v, &diag) && v == 1) {
            case_set_status(ctx, idx, PROBE_PASS,
                "OUTPUT INTO with AFTER trigger works; values are pre-trigger");
        } else {
            ctx->cases[idx].diag = diag;
            case_set_status(ctx, idx, PROBE_FAIL, "row missing after OUTPUT INTO insert");
        }
    } else {
        ctx->cases[idx].diag = diag;
        case_set_status(ctx, idx, PROBE_FAIL, "OUTPUT INTO insert failed");
    }
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK);
    tx_exec(ctx, dbc,
        L"IF OBJECT_ID(N'dbo.tr_vms_probe_tx') IS NOT NULL DROP TRIGGER dbo.tr_vms_probe_tx;"
        L"IF OBJECT_ID(N'dbo.vms_probe_tx') IS NOT NULL DROP TABLE dbo.vms_probe_tx;"
        L"IF OBJECT_ID(N'dbo.vms_probe_log') IS NOT NULL DROP TABLE dbo.vms_probe_log", &diag);
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    SQLDisconnect(dbc); SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    return 1;
}

int cases_tx(ProbeCtx* ctx)
{
    int idx = ctx->case_count;
    ProbeDiag diag;
    HDBC dbc;

    case_add(ctx, "tx", "setup", PROBE_SKIP, NULL);
    if (!ctx->driver_present) {
        case_set_status(ctx, idx, PROBE_SKIP, "driver 18 not present");
        return 1;
    }
    dbc = tx_connect(ctx, &diag, 0);
    if (dbc == SQL_NULL_HDBC) {
        ctx->cases[idx].diag = diag;
        case_set_status(ctx, idx, PROBE_FAIL, "connect (autocommit off) failed");
        return 1;
    }
    case_set_status(ctx, idx, PROBE_PASS, "connected with SQL_ATTR_AUTOCOMMIT=OFF");
    SQLDisconnect(dbc); SQLFreeHandle(SQL_HANDLE_DBC, dbc);

    case_autobegin(ctx);
    case_savepoints(ctx);
    case_xact_state(ctx);
    case_output_into(ctx);

    /* network-loss-during-COMMIT and unknown-COMMIT need fault injection; recorded as designed */
    case_add(ctx, "tx", "unknown_commit_policy", PROBE_SKIP,
             "requires network fault injection; enforced by policy: error + retire HDBC + never retry COMMIT");
    return 5;
}
