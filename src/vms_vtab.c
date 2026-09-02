/* vms_vtab.c — SQLite virtual table module over the client runtime (R6).
 *
 * Module "virtualmssql": CREATE VIRTUAL TABLE x USING virtualmssql(
 *   schema='dbo', table='mytable');
 *
 * Each vtab instance discovers its shape via vms_meta at connect time and
 * declares the matching SQLite schema. Cursors lease their own connections
 * from the pool, so nested scans run concurrently. xColumn maps decoded
 * VmsValue into sqlite3_result_*. Write path (xUpdate) is deliberately
 * absent: R6 is read-only; writes arrive in R10. */
#include "vms_vtab.h"
#include "vms_meta.h"
#include <windows.h>
#include <string.h>
#include <stdio.h>

SQLITE_EXTENSION_INIT3

struct VmsVtabEnv {
    VmsPool* pool;
    VmsProfile profile;
};

struct VmsVtab {
    sqlite3_vtab base;
    VmsVtabEnv* env;
    char schema[128];
    char table[128];
    int ncols;
    VmsMetaColumn* cols;   /* metadata flavor (name/type_name/vtype/...) */
};

struct VmsVtab;

struct VmsVtabCursor {
    sqlite3_vtab_cursor base;
    struct VmsVtab* tab;
    VmsCursor* cur;     /* lease; NULL when the scan finished/closed early */
    int eof;
    sqlite3_int64 rowid_counter;
};

static VmsVtabEnv* g_env = NULL; /* single-module env for R6 */

/* ---------- module helpers ---------- */

static void vtab_set_error(sqlite3_vtab* vtab, const VmsError* err)
{
    if (vtab && err && err->cls != VMS_OK) {
        vtab->zErrMsg = sqlite3_mprintf("virtualmssql: %s (%s)",
                                        err->message, err->sqlstate);
    }
}

/* ---------- xConnect / xCreate ---------- */

static int vms_vtab_connect(sqlite3* db, void* pAux, int argc,
                            const char* const* argv, sqlite3_vtab** ppVtab,
                            char** pzErr)
{
    struct VmsVtab* tab;
    VmsTableColumns cols;
    VmsError err;
    const char* schema = NULL;
    const char* table = NULL;
    char schema_buf[128], table_buf[128];
    char decl[4096];
    size_t dlen = 0;
    int i;
    int rc = SQLITE_ERROR;

    (void)argc;
    if (!g_env) {
        *pzErr = sqlite3_mprintf("virtualmssql: module environment not initialized");
        return SQLITE_ERROR;
    }

    /* argv: [0]=module, [1]=db, [2]=vtab name, [3..]=args */
    for (i = 3; i < argc; i++) {
        const char* a = argv[i];
        if (!strncmp(a, "schema=", 7)) {
            schema = a + 7;
        } else if (!strncmp(a, "table=", 6)) {
            table = a + 6;
        } else {
            *pzErr = sqlite3_mprintf("virtualmssql: unknown argument '%s'", a);
            return SQLITE_ERROR;
        }
    }
    if (!schema || !table) {
        *pzErr = sqlite3_mprintf("virtualmssql: schema= and table= are required");
        return SQLITE_ERROR;
    }
    /* strip surrounding quotes from args */
    {
        size_t n;
        if (schema[0] == '\'' && (n = strlen(schema)) >= 2 && schema[n-1] == '\'') {
            memcpy(schema_buf, schema + 1, n - 2);
            schema_buf[n - 2] = 0;
            schema = schema_buf;
        }
        if (table[0] == '\'' && (n = strlen(table)) >= 2 && table[n-1] == '\'') {
            memcpy(table_buf, table + 1, n - 2);
            table_buf[n - 2] = 0;
            table = table_buf;
        }
    }
    if (!vms_meta_ident_valid(schema, 128) || !vms_meta_ident_valid(table, 128)) {
        *pzErr = sqlite3_mprintf("virtualmssql: invalid identifier");
        return SQLITE_ERROR;
    }

    memset(&err, 0, sizeof(err));
    tab = (struct VmsVtab*)sqlite3_malloc(sizeof(struct VmsVtab));
    if (!tab) return SQLITE_NOMEM;
    memset(tab, 0, sizeof(*tab));
    tab->env = g_env;
    strncpy_s(tab->schema, sizeof(tab->schema), schema, _TRUNCATE);
    strncpy_s(tab->table, sizeof(tab->table), table, _TRUNCATE);

    /* discover the shape through the catalog (R5) via a pool lease */
    {
        VmsConnection* probe = vms_pool_acquire(g_env->pool, &g_env->profile, &err);
        if (!probe) {
            *pzErr = sqlite3_mprintf("virtualmssql: probe lease failed: %s", err.message);
            sqlite3_free(tab);
            return SQLITE_ERROR;
        }
        if (!vms_meta_columns(probe, schema, table, &cols, &err)) {
            vtab_set_error(NULL, &err);
            *pzErr = sqlite3_mprintf("virtualmssql: metadata read failed: %s", err.message);
            vms_pool_release(g_env->pool, probe);
            sqlite3_free(tab);
            return SQLITE_ERROR;
        }
        vms_pool_release(g_env->pool, probe);
    }
    if (cols.count < 1 || cols.count > 512) {
        *pzErr = sqlite3_mprintf("virtualmssql: table has %d columns (unsupported)",
                                 cols.count);
        sqlite3_free(tab);
        return SQLITE_ERROR;
    }
    tab->ncols = cols.count;
    tab->cols = (VmsMetaColumn*)sqlite3_malloc(sizeof(VmsMetaColumn) * (size_t)tab->ncols);
    if (!tab->cols) {
        sqlite3_free(tab);
        return SQLITE_NOMEM;
    }
    memcpy(tab->cols, cols.cols, sizeof(VmsMetaColumn) * (size_t)tab->ncols);
    /* persist the identifiers: they pointed at stack buffers above */
    strncpy_s(tab->schema, sizeof(tab->schema), schema, _TRUNCATE);
    strncpy_s(tab->table, sizeof(tab->table), table, _TRUNCATE);
    for (i = 0; i < tab->ncols; i++) {
    }
    fflush(stderr);

    /* declare schema: SQL Server type -> SQLite affinity via registry.
     * declare_vtab requires a full CREATE TABLE statement. */
    memcpy(decl, "CREATE TABLE x(", 15);
    dlen = 15;
    for (i = 0; i < tab->ncols; i++) {
        const char* affinity;
        switch (tab->cols[i].vtype) {
        case VMS_CT_INT64:  affinity = "INTEGER"; break;
        case VMS_CT_FLOAT64: affinity = "REAL"; break;
        case VMS_CT_BLOB:   affinity = "BLOB"; break;
        default:            affinity = "TEXT"; break; /* TEXT/DECIMAL/DATETIME/GUID/BIGTEXT */
        }
        {
            char col[200];
            _snprintf_s(col, sizeof(col), _TRUNCATE, "%s\"%s\" %s%s",
                        i ? "," : "", tab->cols[i].name, affinity,
                        tab->cols[i].is_nullable ? "" : " NOT NULL");
            if (dlen + strlen(col) + 2 >= sizeof(decl)) {
                *pzErr = sqlite3_mprintf("virtualmssql: schema too wide");
                sqlite3_free(tab->cols);
                sqlite3_free(tab);
                return SQLITE_ERROR;
            }
            memcpy(decl + dlen, col, strlen(col) + 1);
            dlen += strlen(col);
        }
    }
    decl[dlen++] = ')';
    decl[dlen] = 0;
    if (sqlite3_declare_vtab(db, decl) != SQLITE_OK) {
        *pzErr = sqlite3_mprintf("virtualmssql: declare_vtab failed: %s",
                                 sqlite3_errmsg(db));
        sqlite3_free(tab->cols);
        sqlite3_free(tab);
        return SQLITE_ERROR;
    }

    *ppVtab = &tab->base;
    rc = SQLITE_OK;
    return rc;
}

static int vms_vtab_disconnect(sqlite3_vtab* vtab)
{
    struct VmsVtab* tab = (struct VmsVtab*)vtab;
    if (tab->cols) sqlite3_free(tab->cols);
    sqlite3_free(tab);
    return SQLITE_OK;
}

/* ---------- xBestIndex (R6 baseline: full scan) ---------- */

static int vms_vtab_best_index(sqlite3_vtab* vtab, sqlite3_index_info* info)
{
    (void)vtab;
    info->estimatedCost = 1000000.0;
    info->estimatedRows = 100000;
    info->idxNum = 0; /* full scan plan id */
    return SQLITE_OK;
}

/* ---------- xOpen / xClose / xFilter / xNext / xEof / xColumn / xRowid --- */

static int vms_vtab_open(sqlite3_vtab* vtab, sqlite3_vtab_cursor** ppCursor)
{
    struct VmsVtabCursor* cur;
    cur = (struct VmsVtabCursor*)sqlite3_malloc(sizeof(struct VmsVtabCursor));
    if (!cur) return SQLITE_NOMEM;
    memset(cur, 0, sizeof(*cur));
    cur->tab = (struct VmsVtab*)vtab; /* link cursor to its table */
    *ppCursor = &cur->base;
    return SQLITE_OK;
}

static int vms_vtab_close(sqlite3_vtab_cursor* cursor)
{
    struct VmsVtabCursor* cur = (struct VmsVtabCursor*)cursor;
    /* early-close: drop the lease immediately so the pool is not starved */
    if (cur->cur) {
        vms_cursor_close(cur->cur);
        cur->cur = NULL;
    }
    sqlite3_free(cur);
    return SQLITE_OK;
}

static int vms_vtab_filter(sqlite3_vtab_cursor* cursor, int idxNum,
                           const char* idxStr, int argc, sqlite3_value** argv)
{
    struct VmsVtabCursor* cur = (struct VmsVtabCursor*)cursor;
    struct VmsVtab* tab = cur->tab;
    VmsError err;
    VmsConnection* lease;
    (void)idxNum; (void)idxStr; (void)argc; (void)argv;

    memset(&err, 0, sizeof(err));
    /* start a fresh scan on an independent lease from the env pool */
    if (cur->cur) {
        vms_cursor_close(cur->cur);
        cur->cur = NULL;
    }
    lease = vms_pool_acquire(tab->env->pool, &tab->env->profile, &err);
    if (!lease) {
        vtab_set_error(&tab->base, &err);
        return SQLITE_ERROR;
    }
    cur->cur = vms_cursor_open(lease, tab->schema, tab->table,
                               tab->cols, tab->ncols, &err);
    /* the cursor owns its own HDBC; the pool lease connection itself is
     * returned to the pool right away (it only served as the profile
     * source) — the cursor does not hold it */
    vms_pool_release(tab->env->pool, lease);
    if (!cur->cur) {
        vtab_set_error(&tab->base, &err);
        return SQLITE_ERROR;
    }
    cur->eof = 0;
    cur->rowid_counter = 0;
    return vms_vtab_next(cursor); /* position on the first row */
}

static int vms_vtab_next(sqlite3_vtab_cursor* cursor)
{
    struct VmsVtabCursor* cur = (struct VmsVtabCursor*)cursor;
    VmsError err;
    int r;

    if (!cur->cur) {
        cur->eof = 1;
        return SQLITE_OK;
    }
    memset(&err, 0, sizeof(err));
    r = vms_cursor_fetch(cur->cur, &err);
    if (r < 0) {
        vtab_set_error(&cur->tab->base, &err);
        /* the scan is broken: retire the lease */
        vms_cursor_close(cur->cur);
        cur->cur = NULL;
        cur->eof = 1;
        return SQLITE_ERROR;
    }
    if (r == 0) {
        cur->eof = 1;
        /* scan complete: release the lease promptly */
        vms_cursor_close(cur->cur);
        cur->cur = NULL;
        return SQLITE_OK;
    }
    cur->eof = 0;
    cur->rowid_counter++;
    return SQLITE_OK;
}

static int vms_vtab_eof(sqlite3_vtab_cursor* cursor)
{
    return ((struct VmsVtabCursor*)cursor)->eof;
}

static int vms_vtab_column(sqlite3_vtab_cursor* cursor, sqlite3_context* ctx,
                           int col)
{
    struct VmsVtabCursor* cur = (struct VmsVtabCursor*)cursor;
    const VmsValue* v;

    if (!cur->cur || cur->eof) {
        sqlite3_result_null(ctx);
        return SQLITE_OK;
    }
    v = vms_cursor_value(cur->cur, col);
    if (!v || v->type == VMS_VAL_NULL) {
        sqlite3_result_null(ctx);
        return SQLITE_OK;
    }
    switch (v->type) {
    case VMS_VAL_INT64:
        sqlite3_result_int64(ctx, (sqlite3_int64)v->i);
        break;
    case VMS_VAL_FLOAT64:
        sqlite3_result_double(ctx, v->f);
        break;
    case VMS_VAL_TEXT:
        sqlite3_result_text(ctx, v->text, (int)v->text_len, SQLITE_TRANSIENT);
        break;
    case VMS_VAL_BLOB:
        sqlite3_result_blob(ctx, v->blob, (int)v->blob_len, SQLITE_TRANSIENT);
        break;
    default:
        sqlite3_result_null(ctx);
        break;
    }
    return SQLITE_OK;
}

static int vms_vtab_rowid(sqlite3_vtab_cursor* cursor, sqlite3_int64* pRowid)
{
    *pRowid = ((struct VmsVtabCursor*)cursor)->rowid_counter;
    return SQLITE_OK;
}

/* ---------- module struct ---------- */

static sqlite3_module vms_module = {
    1,                 /* iVersion */
    vms_vtab_connect,  /* xCreate (same as connect: no shadow tables) */
    vms_vtab_connect,
    vms_vtab_best_index,
    vms_vtab_disconnect,
    vms_vtab_disconnect, /* xDestroy */
    vms_vtab_open,
    vms_vtab_close,
    vms_vtab_filter,
    vms_vtab_next,
    vms_vtab_eof,
    vms_vtab_column,
    vms_vtab_rowid,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/* ---------- env + registration ---------- */

VmsVtabEnv* vms_vtab_env_create(const VmsProfile* profile, VmsError* err)
{
    VmsVtabEnv* env;
    vms_error_ok(err);
    env = (VmsVtabEnv*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(VmsVtabEnv));
    if (!env) {
        vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM vtab env");
        return NULL;
    }
    env->profile = *profile;
    env->pool = vms_pool_create(4);
    if (!env->pool) {
        vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM vtab pool");
        HeapFree(GetProcessHeap(), 0, env);
        return NULL;
    }
    return env;
}

void vms_vtab_env_destroy(VmsVtabEnv* env)
{
    if (!env) return;
    if (env->pool) vms_pool_destroy(env->pool);
    HeapFree(GetProcessHeap(), 0, env);
}

int vms_vtab_register(sqlite3* db, VmsVtabEnv* env, char** pzErrMsg)
{
    int rc;
    if (!env) {
        if (pzErrMsg) *pzErrMsg = sqlite3_mprintf("virtualmssql: null env");
        return SQLITE_ERROR;
    }
    g_env = env;
    rc = sqlite3_create_module(db, "virtualmssql", &vms_module, NULL);
    return rc;
}
