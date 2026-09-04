/* R1.4 — extension entry point + stub module registration.
 * Loads only when host SQLite meets the baseline; registers a no-op virtual
 * table module and the virtualmssql_version() scalar. */
#include "vms_api.h"
#include "vms_internal.h"
#include "vms_vtab.h"
#include "vms_connstr.h"
#include "vms_credentials.h"
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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

/* profile string for the vtab environment: set via the virtualmssql_profile
 * scalar before CREATE VIRTUAL TABLE, or falls back to the VMS_TEST_PROFILE
 * env (tests). Stored process-wide for R6 single-env scope. */
static char g_profile_spec[1024];
static sqlite3* g_profile_db = NULL;

/* V1031: declared before first use (the profile scalar runs above the
 * definition further down in this file) */
static int vms_vtab_env_init(sqlite3* db, char** pzErrMsg);

static void profile_set_func(sqlite3_context* ctx, int argc, sqlite3_value** argv)
{
    (void)argc;
    if (sqlite3_value_type(argv[0]) != SQLITE_TEXT) {
        sqlite3_result_error(ctx, "virtualmssql_profile expects a text profile", -1);
        return;
    }
    strncpy_s(g_profile_spec, sizeof(g_profile_spec),
              (const char*)sqlite3_value_text(argv[0]), _TRUNCATE);
    /* lazy module registration: the profile must exist before the env can */
    if (g_profile_db) {
        char* vtab_err = NULL;
        if (vms_vtab_env_init(g_profile_db, &vtab_err) != SQLITE_OK) {
            sqlite3_result_error(ctx,
                vtab_err ? vtab_err : "virtualmssql: env init failed", -1);
            if (vtab_err) sqlite3_free(vtab_err);
            return;
        }
        g_profile_db = NULL;
    }
    sqlite3_result_null(ctx);
}

/* scalar: virtualmssql_cred('key', 'secret') — registers a secret with the
 * active provider. Exists so hosts/tests can provision credentials through
 * the DLL itself (the provider state lives inside the DLL). */
static void cred_set_func(sqlite3_context* ctx, int argc, sqlite3_value** argv)
{
    wchar_t key[256];
    wchar_t secret[512];
    if (argc != 2 || sqlite3_value_type(argv[0]) != SQLITE_TEXT ||
        sqlite3_value_type(argv[1]) != SQLITE_TEXT) {
        sqlite3_result_error(ctx, "virtualmssql_cred(key, secret) expects two texts", -1);
        return;
    }
    {
        const unsigned char* k = sqlite3_value_text(argv[0]);
        const unsigned char* s = sqlite3_value_text(argv[1]);
        int nk = MultiByteToWideChar(CP_UTF8, 0, (const char*)k, -1, key, 256);
        int ns = MultiByteToWideChar(CP_UTF8, 0, (const char*)s, -1, secret, 512);
        if (nk <= 0 || ns <= 0) {
            sqlite3_result_error(ctx, "virtualmssql_cred: conversion failed", -1);
            return;
        }
    }
    /* provider must already be installed (load time default = memory) */
    if (!vms_cred_memory_set(key, secret)) {
        sqlite3_result_error(ctx, "virtualmssql_cred: registration failed", -1);
        return;
    }
    sqlite3_result_null(ctx);
}

/* scalar: virtualmssql_cancel() — R14 cancellation entry point. Delivers
 * SQLCancelHandle attention to every live remote connection and interrupts
 * the calling SQLite VM. Returns the number of remote connections signaled
 * (informational; cancellation is best-effort by design). */
static void cancel_func(sqlite3_context* ctx, int argc, sqlite3_value** argv)
{
    int signaled;
    (void)argc;
    (void)argv;
    signaled = vms_client_cancel_all();
    sqlite3_interrupt(sqlite3_context_db_handle(ctx));
    sqlite3_result_int(ctx, signaled);
}

static int vms_vtab_env_init(sqlite3* db, char** pzErrMsg)
{
    VmsProfile profile;
    VmsError err;
    const char* spec = g_profile_spec[0] ? g_profile_spec : getenv("VMS_TEST_PROFILE");
    VmsVtabEnv* env;

    if (!spec || !spec[0]) {
        *pzErrMsg = sqlite3_mprintf(
            "virtualmssql: no connection profile; call "
            "virtualmssql_profile('server=...;auth=...;cred=...') first");
        return SQLITE_ERROR;
    }
    if (!vms_profile_parse(spec, &profile, &err)) {
        *pzErrMsg = sqlite3_mprintf("virtualmssql: bad profile: %s", err.message);
        return SQLITE_ERROR;
    }
    env = vms_vtab_env_create(&profile, &err);
    if (!env) {
        *pzErrMsg = sqlite3_mprintf("virtualmssql: env init failed: %s", err.message);
        return SQLITE_ERROR;
    }
    if (vms_vtab_register(db, env, pzErrMsg) != SQLITE_OK) {
        vms_vtab_env_destroy(env);
        return SQLITE_ERROR;
    }
    return SQLITE_OK;
}

int sqlite3_virtualmssql_init_impl(sqlite3* db, char** pzErrMsg,
                                   const sqlite3_api_routines* pApi)
{
    int cap;
    int sqlite_version;

    /* must precede any sqlite3_* call: they go through the api thunk */
    SQLITE_EXTENSION_INIT2(pApi);
    (void)pzErrMsg;

    /* default credential provider lives inside the DLL (R6: hosts provision
     * secrets through virtualmssql_cred() which writes to this instance) */
    vms_cred_set_provider(vms_cred_memory_provider());

    sqlite_version = sqlite3_libversion_number();

    /* R1.3: no silent downgrade — refuse old hosts explicitly. */
    cap = vms_check_host_capabilities(sqlite_version);
    if (cap != VMS_CAP_OK) {
        const char* diag = vms_capability_diagnostic(cap, sqlite_version);
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
    if (sqlite3_create_function(db, "virtualmssql_profile", 1,
                                SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL,
                                profile_set_func, NULL, NULL) != SQLITE_OK) {
        return SQLITE_ERROR;
    }
    if (sqlite3_create_function(db, "virtualmssql_cred", 2,
                                SQLITE_UTF8, NULL,
                                cred_set_func, NULL, NULL) != SQLITE_OK) {
        return SQLITE_ERROR;
    }
    /* R14: cancellation entry point — delivers attention to every live
     * remote connection (SQLCancelHandle on the active statement) and
     * interrupts the SQLite VM. Callable from any connection/thread. */
    if (sqlite3_create_function(db, "virtualmssql_cancel", 0,
                                SQLITE_UTF8, NULL,
                                cancel_func, NULL, NULL) != SQLITE_OK) {
        return SQLITE_ERROR;
    }
    /* R6: register the real module eagerly when a profile is already
     * available (env var); otherwise it registers lazily inside the
     * virtualmssql_profile() scalar. */
    if (g_profile_spec[0] || getenv("VMS_TEST_PROFILE")) {
        char* vtab_err = NULL;
        if (vms_vtab_env_init(db, &vtab_err) != SQLITE_OK) {
            /* non-fatal: profile scalar can retry */
            g_profile_db = db;
            if (vtab_err) sqlite3_free(vtab_err);
        }
    } else {
        g_profile_db = db;
    }
    return SQLITE_OK;
}

/* ---- R18 public ABI ---- */

int virtualmssql_api_version(void)
{
    return VIRTUALMSSQL_API_VERSION;
}

int virtualmssql_register_credential_provider(const void* provider_v1)
{
    const VmsCredProviderV1* p = (const VmsCredProviderV1*)provider_v1;
    if (!vms_cred_provider_valid(p)) return 1;
    vms_cred_set_provider(p);
    return 0;
}

/* query-profile provider registry (R18): the extension supports one
 * registered provider; its profiles resolve the conn='key' vtab argument
 * against the active env profile (identity check; per-vtab profiles are
 * UNSUPPORTED in the 1.0 single-env scope and fail deterministically). */
static const VmsQueryProfileProviderV1* g_qprofile = NULL;

int virtualmssql_register_query_profile_provider(const void* provider_v1)
{
    const VmsQueryProfileProviderV1* p =
        (const VmsQueryProfileProviderV1*)provider_v1;
    if (!p || p->abi_version != VMS_QPROFILE_PROVIDER_ABI_VERSION ||
        !p->name || !p->get_profile) return 1;
    g_qprofile = p;
    return 0;
}

const void* virtualmssql_wincred_provider(void)
{
    return vms_cred_wincred_provider();
}

int virtualmssql_cancel(sqlite3* db)
{
    int signaled = vms_client_cancel_all();
    if (db) sqlite3_interrupt(db);
    return signaled;
}

/* resolve conn='key' through the registered provider into resolved_spec.
 * Returns 0 on success; 1 unknown key; 2 no provider registered. */
int vms_ext_resolve_qprofile(const char* key, char* resolved_spec, size_t cap)
{
    if (!g_qprofile) return 2;
    if (g_qprofile->get_profile(g_qprofile->ctx, key,
                                resolved_spec, cap) != 0) return 1;
    return 0;
}

VMS_EXPORT int sqlite3_virtualmssql_init(sqlite3* db, char** pzErrMsg,
                                         const sqlite3_api_routines* pApi)
{
    return sqlite3_virtualmssql_init_impl(db, pzErrMsg, pApi);
}

VMS_EXPORT int sqlite3_extension_init(sqlite3* db, char** pzErrMsg,
                                      const sqlite3_api_routines* pApi)
{
    return sqlite3_virtualmssql_init_impl(db, pzErrMsg, pApi);
}
