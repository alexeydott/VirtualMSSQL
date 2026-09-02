/* R1.4 — extension entry point + stub module registration.
 * Loads only when host SQLite meets the baseline; registers a no-op virtual
 * table module and the virtualmssql_version() scalar. */
#include "vms_api.h"
#include "vms_internal.h"
#include <string.h>
#include <stdio.h>

/* redirect sqlite3_* calls through the api-routines thunk */
#ifndef SQLITE_CORE
SQLITE_EXTENSION_INIT1
#endif

/* ---- stub virtual table module: one no-op eponymous module ---- */

typedef struct VmsStubVtab {
    sqlite3_vtab base;
} VmsStubVtab;

typedef struct VmsStubCursor {
    sqlite3_vtab_cursor base;
    int eof;
} VmsStubCursor;

static int stub_connect(sqlite3* db, void* pAux, int argc, const char* const* argv,
                        sqlite3_vtab** ppVtab, char** pzErr)
{
    VmsStubVtab* vtab;
    (void)pAux; (void)argc; (void)argv; (void)pzErr;
    vtab = (VmsStubVtab*)sqlite3_malloc(sizeof(VmsStubVtab));
    if (!vtab) return SQLITE_NOMEM;
    memset(vtab, 0, sizeof(*vtab));
    if (sqlite3_declare_vtab(db, "CREATE TABLE x(msg TEXT)") != SQLITE_OK) {
        sqlite3_free(vtab);
        return SQLITE_ERROR;
    }
    *ppVtab = &vtab->base;
    return SQLITE_OK;
}

static int stub_disconnect(sqlite3_vtab* vtab)
{
    sqlite3_free(vtab);
    return SQLITE_OK;
}

static int stub_open(sqlite3_vtab* vtab, sqlite3_vtab_cursor** ppCursor)
{
    VmsStubCursor* cur;
    (void)vtab;
    cur = (VmsStubCursor*)sqlite3_malloc(sizeof(VmsStubCursor));
    if (!cur) return SQLITE_NOMEM;
    memset(cur, 0, sizeof(*cur));
    cur->eof = 1; /* no rows in the stub */
    *ppCursor = &cur->base;
    return SQLITE_OK;
}

static int stub_close(sqlite3_vtab_cursor* cursor)
{
    sqlite3_free(cursor);
    return SQLITE_OK;
}

static int stub_eof(sqlite3_vtab_cursor* cursor)
{
    return ((VmsStubCursor*)cursor)->eof;
}

static int stub_next(sqlite3_vtab_cursor* cursor)
{
    ((VmsStubCursor*)cursor)->eof = 1;
    return SQLITE_OK;
}

static int stub_rowid(sqlite3_vtab_cursor* cursor, sqlite_int64* pRowid)
{
    (void)cursor;
    *pRowid = 0;
    return SQLITE_OK;
}

static int stub_column(sqlite3_vtab_cursor* cursor, sqlite3_context* ctx, int col)
{
    (void)cursor;
    if (col == 0) {
        sqlite3_result_text(ctx, "vms-stub", -1, SQLITE_STATIC);
    } else {
        sqlite3_result_null(ctx);
    }
    return SQLITE_OK;
}

static sqlite3_module vms_stub_module = {
    0,                 /* iVersion */
    0,                 /* xCreate (eponymous-only) */
    stub_connect,
    0,                 /* xBestIndex — set to stub_best_index at init */
    stub_disconnect,
    0,                 /* xDestroy */
    stub_open,
    stub_close,
    0,                 /* xFilter — set to stub_filter at init */
    stub_next,
    stub_eof,
    stub_column,
    stub_rowid,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/* xBestIndex stub: full scan, no constraints consumed. */
static int stub_best_index(sqlite3_vtab* tab, sqlite3_index_info* info)
{
    (void)tab;
    info->estimatedCost = 1000000.0;
    info->estimatedRows = 25;
    return SQLITE_OK;
}

/* xFilter stub: start at eof. */
static int stub_filter(sqlite3_vtab_cursor* cur, int idxNum, const char* idxStr,
                       int argc, sqlite3_value** argv)
{
    (void)cur; (void)idxNum; (void)idxStr; (void)argc;
    ((VmsStubCursor*)cur)->eof = 1;
    return SQLITE_OK;
}

static void version_func(sqlite3_context* ctx, int argc, sqlite3_value** argv)
{
    (void)argc; (void)argv;
    sqlite3_result_text(ctx, VMS_PROJECT_NAME " " VMS_PROJECT_VERSION, -1, SQLITE_STATIC);
}

static const char* kStubSchema =
    "CREATE TABLE IF NOT EXISTS virtualmssql_stub(msg TEXT)";

int sqlite3_virtualmssql_init_impl(sqlite3* db, char** pzErrMsg,
                                   const sqlite3_api_routines* pApi)
{
    int cap;
    int sqlite_version;

    /* must precede any sqlite3_* call: they go through the api thunk */
    SQLITE_EXTENSION_INIT2(pApi);
    (void)pzErrMsg;

    sqlite_version = sqlite3_libversion_number();

    /* R1.3: no silent downgrade — refuse old hosts explicitly. */
    cap = vms_check_host_capabilities(sqlite_version);
    if (cap != VMS_CAP_OK) {
        const char* diag = vms_capability_diagnostic(cap, sqlite_version);
        fprintf(stderr, "%s\n", diag);
        return SQLITE_ERROR;
    }

    vms_stub_module.xBestIndex = stub_best_index;
    vms_stub_module.xFilter = stub_filter;

    if (sqlite3_create_module(db, "virtualmssql_stub", &vms_stub_module, NULL) != SQLITE_OK) {
        return SQLITE_ERROR;
    }
    if (sqlite3_create_function(db, "virtualmssql_version", 0,
                                SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL,
                                version_func, NULL, NULL) != SQLITE_OK) {
        return SQLITE_ERROR;
    }
    return SQLITE_OK;
}

int sqlite3_virtualmssql_init(sqlite3* db, char** pzErrMsg,
                              const sqlite3_api_routines* pApi)
{
    return sqlite3_virtualmssql_init_impl(db, pzErrMsg, pApi);
}

int sqlite3_extension_init(sqlite3* db, char** pzErrMsg,
                           const sqlite3_api_routines* pApi)
{
    return sqlite3_virtualmssql_init_impl(db, pzErrMsg, pApi);
}
