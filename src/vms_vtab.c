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
#include "vms_meta_cache.h"
#include "vms_plan.h"
#include "vms_query_source.h"
#include "vms_mat.h"
#include "vms_dml.h"
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

SQLITE_EXTENSION_INIT3

#define VMS_TXN_MAX_SAVEPOINTS 32

struct VmsVtabEnv {
    VmsPool* pool;
    VmsProfile profile;
    /* R13: metadata cache (process-wide shadow storage) */
    VmsMetaCache* mdcache;
    CRITICAL_SECTION cache_cs;
    int cache_cs_init;
    /* R11: pinned transaction connection (one canonical identity per
     * transaction). NULL when no explicit transaction is open. */
    CRITICAL_SECTION txn_cs;
    int txn_cs_init;
    VmsConnection* txn_cn;
    int txn_pinned;                  /* pinned + primer applied */
    /* savepoint bookkeeping (SQLite per-transaction savepoint ids) */
    char sv_names[VMS_TXN_MAX_SAVEPOINTS][48];
    int sv_count;
};

#define VMS_TXN_MAX_SAVEPOINTS 32

struct VmsVtab {
    sqlite3_vtab base;
    VmsVtabEnv* env;
    char schema[128];
    char table[128];
    int ncols;
    VmsMetaColumn* cols;   /* metadata flavor (name/type_name/vtype/...) */
    /* R8: query source mode */
    int is_query_source;
    VmsQuerySource qsrc;
    char query_spec[32768];
    /* R9: materialization (query sources only) */
    VmsMatMode mat_mode;
    VmsMat* mat;           /* published snapshot; guarded by SQLite core */
    /* R10: write mode (table sources only) */
    int rw_mode;
    VmsDmlContext* dml;    /* prepared lazily; requires a stable key */
    /* R11: DML context bound to the pinned transaction connection */
    VmsDmlContext* dml_txn;
    /* R12: spatial columns rendered as WKT (1) or WKB (0, default) */
    int spatial_wkt;
    /* R13: metadata_mode: 0 = live (default), 1 = cached */
    int metadata_cached;
    unsigned long long schema_fp;   /* fingerprint of the live capture */
    long long captured_utc;         /* capture timestamp of the cache entry */
    /* R10: stable-key values of the last row positioned by xRowid (stash
     * for xUpdate WHERE clauses: DELETE has no cursor access in xUpdate,
     * but SQLite always calls xRowid right before DELETE/UPDATE) */
    char key_text[VMS_META_MAX_KEY_PARTS][160];
    int key_part_col[VMS_META_MAX_KEY_PARTS]; /* key part -> vtab col index */
    int key_have;
};

struct VmsVtab;

struct VmsVtabCursor {
    sqlite3_vtab_cursor base;
    struct VmsVtab* tab;
    VmsCursor* cur;     /* lease; NULL when the scan finished/closed early */
    int eof;
    sqlite3_int64 rowid_counter;
    /* R9 snapshot scan state */
    sqlite3* snapshot_db;
    sqlite3_stmt* snapshot_stmt;
    /* R7: map vtab column index -> position in the remote projection.
     * -1 = column not projected (xColumn must not read it, but SQLite only
     * calls xColumn for colUsed columns which are all projected). */
    int col_map[512];
};

static VmsVtabEnv* g_env = NULL; /* single-module env for R6 */

/* R11 forward decls (used by xFilter / best_index before their definitions) */
static VmsConnection* txn_current(VmsVtabEnv* env);
static int vtab_dml_ctx(struct VmsVtab* tab, VmsDmlContext** out, VmsError* err);

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
    const char* query = NULL;
    char schema_buf[128], table_buf[128], query_buf[32768];
    char decl[4096];
    size_t dlen = 0;
    int i;
    int rc = SQLITE_ERROR;
    int is_query = 0;
    int rw = 0;
    int mat_mode_parsed = VMS_MAT_OFF;
    int spatial_wkt = 0; /* R12: default spatial representation = WKB */
    int metadata_cached = 0; /* R13: default metadata_mode = live */

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
        } else if (!strncmp(a, "source=", 7)) {
            const char* sv = a + 7;
            if (sv[0] == '\'') sv++; /* strip leading quote */
            if (strncmp(sv, "query", 5) && strncmp(sv, "table", 5)) {
                *pzErr = sqlite3_mprintf("virtualmssql: source must be 'table' or 'query'");
                return SQLITE_ERROR;
            }
            is_query = !strncmp(sv, "query", 5);
        } else if (!strncmp(a, "mode=", 5)) {
            const char* mv = a + 5;
            if (mv[0] == '\'') mv++;
            if (strncmp(mv, "rw", 2) && strncmp(mv, "ro", 2) &&
                strncmp(mv, "rw'", 3) && strncmp(mv, "ro'", 3)) {
                *pzErr = sqlite3_mprintf("virtualmssql: mode must be 'ro' or 'rw'");
                return SQLITE_ERROR;
            }
            rw = !strncmp(mv, "rw", 2);
        } else if (!strncmp(a, "query=", 6)) {
            query = a + 6;
        } else if (!strncmp(a, "materialization=", 16)) {
            const char* mv = a + 16;
            char mvbuf[32];
            size_t n;
            if (mv[0] == '\'') mv++; /* strip leading quote */
            n = strlen(mv);
            if (n > 0 && mv[n - 1] == '\'') n--; /* strip trailing quote */
            if (n >= sizeof(mvbuf)) n = sizeof(mvbuf) - 1;
            memcpy(mvbuf, mv, n);
            mvbuf[n] = 0;
            mat_mode_parsed = vms_mat_mode_parse(mvbuf);
            if (mat_mode_parsed < 0) {
                *pzErr = sqlite3_mprintf("virtualmssql: materialization must be off|memory|temp");
                return SQLITE_ERROR;
            }
        } else if (!strncmp(a, "spatial=", 8)) {
            /* R12: spatial UDT representation: wkb (default) or wkt */
            const char* sv = a + 8;
            if (sv[0] == '\'') sv++;
            if (!strncmp(sv, "wkt", 3)) spatial_wkt = 1;
            else if (!strncmp(sv, "wkb", 3)) spatial_wkt = 0;
            else {
                *pzErr = sqlite3_mprintf("virtualmssql: spatial must be 'wkb' or 'wkt'");
                return SQLITE_ERROR;
            }
        } else if (!strncmp(a, "metadata_mode=", 14)) {
            /* R13: live (default) reads metadata from the server on every
             * connect; cached consults the shadow cache and applies the
             * live validation policy (fresh / stale / drift). */
            const char* sv = a + 14;
            if (sv[0] == '\'') sv++;
            if (!strncmp(sv, "cached", 6)) metadata_cached = 1;
            else if (!strncmp(sv, "live", 4)) metadata_cached = 0;
            else {
                *pzErr = sqlite3_mprintf("virtualmssql: metadata_mode must be 'live' or 'cached'");
                return SQLITE_ERROR;
            }
        } else {
            *pzErr = sqlite3_mprintf("virtualmssql: unknown argument '%s'", a);
            return SQLITE_ERROR;
        }
    }
    if (is_query && !query) {
        *pzErr = sqlite3_mprintf("virtualmssql: source=query requires query='...'");
        return SQLITE_ERROR;
    }
    if (!is_query && (!schema || !table)) {
        *pzErr = sqlite3_mprintf("virtualmssql: schema= and table= are required");
        return SQLITE_ERROR;
    }
    /* strip surrounding quotes from args */
    {
        size_t n;
        if (schema && schema[0] == '\'' && (n = strlen(schema)) >= 2 && schema[n-1] == '\'') {
            memcpy(schema_buf, schema + 1, n - 2);
            schema_buf[n - 2] = 0;
            schema = schema_buf;
        }
        if (table && table[0] == '\'' && (n = strlen(table)) >= 2 && table[n-1] == '\'') {
            memcpy(table_buf, table + 1, n - 2);
            table_buf[n - 2] = 0;
            table = table_buf;
        }
        if (query && query[0] == '\'' && (n = strlen(query)) >= 2 && query[n-1] == '\'') {
            memcpy(query_buf, query + 1, n - 2);
            query_buf[n - 2] = 0;
            query = query_buf;
        }
    }
    if (!is_query &&
        (!vms_meta_ident_valid(schema, 128) || !vms_meta_ident_valid(table, 128))) {
        *pzErr = sqlite3_mprintf("virtualmssql: invalid identifier");
        return SQLITE_ERROR;
    }
    /* R13: shadow names are private to the module */
    if (table && vms_vtab_shadow_name(table)) {
        *pzErr = sqlite3_mprintf(
            "virtualmssql: '%s' is a reserved shadow name (module-private)",
            table);
        return SQLITE_ERROR;
    }

    memset(&err, 0, sizeof(err));
    tab = (struct VmsVtab*)sqlite3_malloc(sizeof(struct VmsVtab));
    if (!tab) return SQLITE_NOMEM;
    memset(tab, 0, sizeof(*tab));
    tab->env = g_env;
    tab->is_query_source = is_query;
    if (is_query) {
        strncpy_s(tab->query_spec, sizeof(tab->query_spec), query, _TRUNCATE);
    } else {
        strncpy_s(tab->schema, sizeof(tab->schema), schema, _TRUNCATE);
        strncpy_s(tab->table, sizeof(tab->table), table, _TRUNCATE);
    }

    /* discover the shape: live probe vs cached shadow read (R13) */
    {
        VmsConnection* probe = NULL;
        int server_ok = 0;
        unsigned long long live_fp = 0;

        if (!metadata_cached) {
            /* live mode: every connect reads the server (default) */
            probe = vms_pool_acquire(g_env->pool, &g_env->profile, &err);
            if (!probe) {
                *pzErr = sqlite3_mprintf("virtualmssql: probe lease failed: %s", err.message);
                sqlite3_free(tab);
                return SQLITE_ERROR;
            }
            if (is_query) {
                if (!vms_query_source_prepare(probe, query, &tab->qsrc, &err)) {
                    *pzErr = sqlite3_mprintf("virtualmssql: query rejected: %s", err.message);
                    vms_pool_release(g_env->pool, probe);
                    sqlite3_free(tab);
                    return SQLITE_ERROR;
                }
                cols.count = 0; /* shape comes from qsrc */
            } else if (!vms_meta_columns(probe, schema, table, &cols, &err)) {
                *pzErr = sqlite3_mprintf("virtualmssql: metadata read failed: %s", err.message);
                vms_pool_release(g_env->pool, probe);
                sqlite3_free(tab);
                return SQLITE_ERROR;
            }
            vms_pool_release(g_env->pool, probe);
        } else {
            /* cached mode: consult the shadow cache first */
            VmsCacheResult cres;
            int cached_count = 0;
            VmsTableColumns cached_cols;

            /* 1) try the cache without a server round-trip */
            EnterCriticalSection(&g_env->cache_cs);
            cres = vms_meta_cache_get(g_env->mdcache, schema, table,
                                      &cached_cols, &cached_count, 0);
            LeaveCriticalSection(&g_env->cache_cs);
            if (cres == VMS_CACHE_CORRUPT) {
                *pzErr = sqlite3_mprintf(
                    "virtualmssql: corrupt metadata cache entry for '%s.%s'",
                    schema, table);
                sqlite3_free(tab);
                return SQLITE_ERROR;
            }

            /* 2) validate live: reachable server decides fresh vs stale */
            probe = vms_pool_acquire(g_env->pool, &g_env->profile, &err);
            if (probe) {
                if (!is_query && vms_meta_columns(probe, schema, table, &cols, &err)) {
                    server_ok = 1;
                    live_fp = vms_meta_fingerprint(&cols, cols.count);
                } else if (!is_query) {
                    /* live read failed while the cache has an entry:
                     * fall back to the stale snapshot */
                    memset(&err, 0, sizeof(err));
                }
                vms_pool_release(g_env->pool, probe);
            }

            if (is_query) {
                /* query sources are always described live (shape depends on
                 * the statement, not a stable object) */
                probe = vms_pool_acquire(g_env->pool, &g_env->profile, &err);
                if (!probe) {
                    *pzErr = sqlite3_mprintf("virtualmssql: probe lease failed: %s", err.message);
                    sqlite3_free(tab);
                    return SQLITE_ERROR;
                }
                if (!vms_query_source_prepare(probe, query, &tab->qsrc, &err)) {
                    *pzErr = sqlite3_mprintf("virtualmssql: query rejected: %s", err.message);
                    vms_pool_release(g_env->pool, probe);
                    sqlite3_free(tab);
                    return SQLITE_ERROR;
                }
                vms_pool_release(g_env->pool, probe);
                cols.count = 0;
            } else if (server_ok) {
                if (cres == VMS_CACHE_MISS) {
                    /* first capture: store the shadow snapshot */
                    EnterCriticalSection(&g_env->cache_cs);
                    vms_meta_cache_put(g_env->mdcache, schema, table, &cols,
                                       cols.count, live_fp, (long long)time(NULL));
                    LeaveCriticalSection(&g_env->cache_cs);
                } else if (live_fp != 0) {
                    VmsTableColumns entry_cols;
                    int entry_count = 0;
                    EnterCriticalSection(&g_env->cache_cs);
                    cres = vms_meta_cache_get(g_env->mdcache, schema, table,
                                              &entry_cols, &entry_count, live_fp);
                    if (cres == VMS_CACHE_DRIFT) {
                        LeaveCriticalSection(&g_env->cache_cs);
                        *pzErr = sqlite3_mprintf(
                            "virtualmssql: SCHEMA_DRIFT on '%s.%s': cached "
                            "metadata no longer matches the server; drop and "
                            "recreate the virtual table",
                            schema, table);
                        vms_meta_cache_drop(g_env->mdcache, schema, table);
                        sqlite3_free(tab);
                        return SQLITE_ERROR;
                    }
                    if (cres == VMS_CACHE_CORRUPT) {
                        LeaveCriticalSection(&g_env->cache_cs);
                        *pzErr = sqlite3_mprintf(
                            "virtualmssql: corrupt metadata cache entry for '%s.%s'",
                            schema, table);
                        sqlite3_free(tab);
                        return SQLITE_ERROR;
                    }
                    LeaveCriticalSection(&g_env->cache_cs);
                }
            } else {
                /* server unavailable: stale read allowed only with an entry */
                if (cres == VMS_CACHE_MISS || cres == VMS_CACHE_DRIFT) {
                    *pzErr = sqlite3_mprintf(
                        "virtualmssql: server unavailable and no usable cached "
                        "metadata for '%s.%s': %s", schema, table, err.message);
                    sqlite3_free(tab);
                    return SQLITE_ERROR;
                }
                memcpy(&cols, &cached_cols, sizeof(cols));
                tab->captured_utc = -1; /* flagged stale; fingerprint unknown */
            }
            tab->schema_fp = live_fp;
        }
        if (!metadata_cached && !is_query) {
            tab->schema_fp = vms_meta_fingerprint(&cols, cols.count);
            tab->captured_utc = (long long)time(NULL);
        }
    }
    tab->metadata_cached = metadata_cached;
    tab->ncols = is_query ? tab->qsrc.ncols : cols.count;
    if (tab->ncols < 1 || tab->ncols > 512) {
        *pzErr = sqlite3_mprintf("virtualmssql: object has %d columns (unsupported)",
                                 tab->ncols);
        sqlite3_free(tab);
        return SQLITE_ERROR;
    }
    if (is_query) {
        tab->mat_mode = (VmsMatMode)mat_mode_parsed;
        if (rw) {
            *pzErr = sqlite3_mprintf(
                "virtualmssql: mode=rw is only valid for source=table");
            sqlite3_free(tab);
            return SQLITE_ERROR;
        }
    } else {
        tab->rw_mode = rw;
    }
    tab->cols = (VmsMetaColumn*)sqlite3_malloc(sizeof(VmsMetaColumn) * (size_t)tab->ncols);
    if (!tab->cols) {
        sqlite3_free(tab);
        return SQLITE_NOMEM;
    }
    if (is_query) {
        memcpy(tab->cols, tab->qsrc.cols, sizeof(VmsMetaColumn) * (size_t)tab->ncols);
    } else {
        memcpy(tab->cols, cols.cols, sizeof(VmsMetaColumn) * (size_t)tab->ncols);
        /* persist the identifiers: they pointed at stack buffers above */
        strncpy_s(tab->schema, sizeof(tab->schema), schema, _TRUNCATE);
        strncpy_s(tab->table, sizeof(tab->table), table, _TRUNCATE);
    }
    tab->spatial_wkt = spatial_wkt;

    /* R12: deterministic UNSUPPORTED_TYPE — a table whose columns contain
     * types with no lossless mapping is rejected at CREATE time */
    for (i = 0; i < tab->ncols; i++) {
        if (tab->cols[i].vtype == VMS_CT_UNSUPPORTED) {
            *pzErr = sqlite3_mprintf(
                "virtualmssql: UNSUPPORTED_TYPE: column '%s' of '%s.%s' has "
                "SQL Server type '%s' with no lossless mapping",
                tab->cols[i].name, tab->schema, tab->table,
                tab->cols[i].type_name);
            sqlite3_free(tab->cols);
            sqlite3_free(tab);
            return SQLITE_ERROR;
        }
    }

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
        case VMS_CT_SPATIAL: affinity = tab->spatial_wkt ? "TEXT" : "BLOB"; break;
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
    if (tab->is_query_source) vms_query_source_free(&tab->qsrc);
    if (tab->mat) vms_mat_destroy(tab->mat);
    if (tab->dml) {
        /* the DML context owns its dedicated lease connection */
        if (tab->dml->cn) vms_conn_close(tab->dml->cn);
        HeapFree(GetProcessHeap(), 0, tab->dml);
    }
    if (tab->dml_txn) {
        /* the transaction connection is owned by the env pin */
        HeapFree(GetProcessHeap(), 0, tab->dml_txn);
    }
    if (tab->cols) sqlite3_free(tab->cols);
    sqlite3_free(tab);
    return SQLITE_OK;
}

/* ---------- xBestIndex (R7: safe pushdown) ---------- */

static int vms_vtab_best_index(sqlite3_vtab* vtab, sqlite3_index_info* info)
{
    struct VmsVtab* tab = (struct VmsVtab*)vtab;
    VmsPlan plan;
    char buf[sizeof(VmsPlan)];

    /* R8: query sources run full scans only (pushdown into an arbitrary
     * SELECT is deferred; the outer wrapper already guarantees the shape) */
    if (tab->is_query_source) {
        info->estimatedCost = 100000.0;
        info->estimatedRows = 1000;
        info->idxNum = 2; /* query source full scan */
        return SQLITE_OK;
    }
    if (!vms_plan_compile(info, tab->cols, tab->ncols, &plan)) {
        info->estimatedCost = 1000000.0;
        info->estimatedRows = 100000;
        info->idxNum = 0;
        return SQLITE_OK;
    }
    /* R10: for writable tables the stable-key columns must always be part of
     * the projection (xRowid stashes them for xUpdate WHERE clauses) */
    if (tab->rw_mode) {
        VmsDmlContext* d;
        VmsError derr;
        memset(&derr, 0, sizeof(derr));
        if (vtab_dml_ctx(tab, &d, &derr)) {
            int k, j;
            for (k = 0; k < d->key.part_count; k++) {
                for (j = 0; j < tab->ncols; j++) {
                    if (!_stricmp(tab->cols[j].name, d->key.parts[k].name) &&
                        j < 62) {
                        plan.used_mask |= (1 << j);
                    }
                }
            }
        }
    }
    if (!vms_plan_serialize(&plan, buf, sizeof(buf))) {
        info->estimatedCost = 1000000.0;
        info->estimatedRows = 100000;
        info->idxNum = 0;
        return SQLITE_OK;
    }
    info->idxNum = 1; /* pushed-down plan */
    info->idxStr = (char*)sqlite3_malloc(sizeof(VmsPlan));
    if (!info->idxStr) return SQLITE_NOMEM;
    memcpy(info->idxStr, buf, sizeof(VmsPlan));
    info->needToFreeIdxStr = 1;
    info->estimatedCost = plan.nterms > 0 ? 100.0 : 100000.0;
    info->estimatedRows = 1000;
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
    if (cur->snapshot_stmt) {
        sqlite3_finalize(cur->snapshot_stmt);
        cur->snapshot_stmt = NULL;
    }
    cur->snapshot_db = NULL;
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
    wchar_t sql[2048];
    long long plan_limit = -1;
    long long plan_offset = 0;
    long long term_params[VMS_PLAN_MAX_ARGS];
    VmsPlan plan;
    int have_plan = 0;

    memset(&err, 0, sizeof(err));
    /* start a fresh scan on an independent lease from the env pool */
    if (cur->cur) {
        vms_cursor_close(cur->cur);
        cur->cur = NULL;
    }

    if (idxNum == 1 && idxStr && !tab->is_query_source &&
        vms_plan_deserialize(idxStr, sizeof(VmsPlan), &plan)) {
        have_plan = 1;
    }
    {
        int a;
        for (a = 0; a < VMS_PLAN_MAX_ARGS; a++) term_params[a] = 0;
    }

    if (have_plan) {
        /* map argv values to plan order; only safe value kinds are bound.
         * term_params[a] receives the bound value for term a (0 = none). */
        int a;
        for (a = 0; a < plan.nterms && a < VMS_PLAN_MAX_ARGS; a++) term_params[a] = 0;
        for (a = 0; a < plan.nterms; a++) {
            VmsPlanTerm* t = &plan.terms[a];
            sqlite3_value* v = (t->arg_index >= 0 && t->arg_index < argc)
                               ? argv[t->arg_index] : NULL;
            if (t->op == VMS_OP_ISNULL || t->op == VMS_OP_ISNOTNULL) continue;
            if (t->op == VMS_OP_IN) {
                /* expand the bounded integer list via vtab_in_first/next.
                 * Multi-value IN is NOT pushed (correctness first): it is
                 * replaced by a contradiction marker (1=0) unless all values
                 * are the same single integer, in which case it degrades to
                 * equality. */
                sqlite3_value* item = NULL;
                long long items[64];
                int nitems = 0;
                int rc2 = sqlite3_vtab_in_first(v, &item);
                while (rc2 == SQLITE_OK && item && nitems < 64) {
                    if (sqlite3_value_numeric_type(item) == SQLITE_INTEGER) {
                        items[nitems++] = sqlite3_value_int64(item);
                    }
                    rc2 = sqlite3_vtab_in_next(v, &item);
                }
                if (nitems == 0) {
                    plan.terms[a].col = -1; /* marker: contradiction */
                } else {
                    int same = 1;
                    int k;
                    for (k = 1; k < nitems; k++) {
                        if (items[k] != items[0]) { same = 0; break; }
                    }
                    if (!same) {
                        plan.terms[a].col = -2; /* marker: unsatisfiable */
                    } else {
                        term_params[a] = items[0];
                    }
                }
                continue;
            }
            /* comparison values must be integers (planner gated on column
             * affinity; value affinity is re-checked here) */
            if (v && sqlite3_value_numeric_type(v) == SQLITE_INTEGER) {
                term_params[a] = sqlite3_value_int64(v);
            } else {
                /* planner omitted the constraint but the value is not an
                 * integer: fail loudly rather than return wrong data */
                tab->base.zErrMsg = sqlite3_mprintf(
                    "virtualmssql: non-integer value for pushed-down "
                    "constraint (plan/arg mismatch)");
                return SQLITE_ERROR;
            }
        }
        /* limit/offset: they occupy the tail argv slots */
        if (plan.has_limit && plan.limit_arg >= 0 && plan.limit_arg < argc) {
            if (sqlite3_value_numeric_type(argv[plan.limit_arg]) == SQLITE_INTEGER)
                plan_limit = sqlite3_value_int64(argv[plan.limit_arg]);
            else
                plan_limit = -1;
        }
        if (plan.has_offset && plan.offset_arg >= 0 && plan.offset_arg < argc) {
            if (sqlite3_value_numeric_type(argv[plan.offset_arg]) == SQLITE_INTEGER)
                plan_offset = sqlite3_value_int64(argv[plan.offset_arg]);
            else
                plan_offset = 0;
        }
        if (plan.has_limit && plan_limit < 0) {
            plan.has_limit = 0;
            plan.has_offset = 0;
        }
    }

    lease = vms_pool_acquire(tab->env->pool, &tab->env->profile, &err);
    if (!lease) {
        vtab_set_error(&tab->base, &err);
        return SQLITE_ERROR;
    }
    if (tab->is_query_source) {
        /* R9 materialized mode: build once, then read from the private db.
         * Build happens inside xFilter on first scan; a FAILED build
         * propagates the error; partial snapshots are never published
         * (vms_mat_build flips to PUBLISHED only on full success). */
        if (tab->mat_mode != VMS_MAT_OFF) {
            if (!tab->mat) {
                tab->mat = vms_mat_create(tab->mat_mode, 0, 0);
                if (!tab->mat) {
                    vms_pool_release(tab->env->pool, lease);
                    tab->base.zErrMsg = sqlite3_mprintf(
                        "virtualmssql: materializer init failed");
                    return SQLITE_ERROR;
                }
                if (!vms_mat_build(tab->mat, lease, &tab->qsrc, &err)) {
                    vms_mat_destroy(tab->mat);
                    tab->mat = NULL;
                    vms_pool_release(tab->env->pool, lease);
                    vtab_set_error(&tab->base, &err);
                    return SQLITE_ERROR;
                }
            }
            vms_pool_release(tab->env->pool, lease);
            /* snapshot scan: rows come from the private published db */
            cur->snapshot_db = vms_mat_db(tab->mat);
            cur->snapshot_stmt = NULL;
            if (cur->snapshot_db) {
                char q[256];
                sqlite3_stmt* st = NULL;
                _snprintf_s(q, sizeof(q), _TRUNCATE,
                            "SELECT * FROM %s", vms_mat_table_name());
                if (sqlite3_prepare_v2(cur->snapshot_db, q, -1, &st, NULL) == SQLITE_OK) {
                    cur->snapshot_stmt = st;
                } else {
                    cur->snapshot_db = NULL;
                    tab->base.zErrMsg = sqlite3_mprintf(
                        "virtualmssql: snapshot read failed");
                    return SQLITE_ERROR;
                }
            }
            cur->eof = 0;
            cur->rowid_counter = 0;
            {
                int vcol;
                for (vcol = 0; vcol < tab->ncols && vcol < 512; vcol++)
                    cur->col_map[vcol] = vcol;
            }
            return vms_vtab_next(cursor);
        }
        /* R8 streaming mode: validated query scan — no pushdown in this
         * mode; the query is used as-is (a WITH head is legal at statement
         * level but not inside a derived table, so no outer wrapper) */
        {
            wchar_t qsql[34816];
            if (!vms_query_source_get_sql(&tab->qsrc, qsql, 34816)) {
                vms_pool_release(tab->env->pool, lease);
                tab->base.zErrMsg = sqlite3_mprintf("virtualmssql: query copy failed");
                return SQLITE_ERROR;
            }
            cur->cur = vms_cursor_open_sql(lease, qsql, NULL, 0, &err);
            vms_pool_release(tab->env->pool, lease);
            if (!cur->cur) {
                vtab_set_error(&tab->base, &err);
                return SQLITE_ERROR;
            }
            cur->eof = 0;
            cur->rowid_counter = 0;
            {
                int vcol;
                for (vcol = 0; vcol < tab->ncols && vcol < 512; vcol++)
                    cur->col_map[vcol] = vcol; /* full projection */
            }
            return vms_vtab_next(cursor);
        }
    }
    if (have_plan) {
        /* rebuild SQL from the (possibly adjusted) plan; parameter order in
         * the SQL text is: TOP(?) first (no ORDER BY case), then WHERE ?s,
         * then OFFSET/FETCH ?s */
        int np = 0;
        long long sqlparams[VMS_PLAN_MAX_ARGS];
        int sp = 0;
        int a;
        if (!vms_plan_build_sql(&plan, tab->schema, tab->table,
                                tab->cols, tab->ncols, tab->spatial_wkt,
                                sql, 2048, &np)) {
            vms_pool_release(tab->env->pool, lease);
            tab->base.zErrMsg = sqlite3_mprintf("virtualmssql: plan SQL build failed");
            return SQLITE_ERROR;
        }
        /* SQL order: TOP(?) when limit and no order */
        if (plan.has_limit && plan.norder == 0 && !plan.has_offset) {
            sqlparams[sp++] = plan_limit;
        }
        for (a = 0; a < plan.nterms; a++) {
            VmsPlanTerm* t = &plan.terms[a];
            if (t->op == VMS_OP_ISNULL || t->op == VMS_OP_ISNOTNULL) continue;
            if (t->col < 0) continue; /* contradiction: no parameter */
            sqlparams[sp++] = term_params[a];
        }
        if (plan.has_offset && plan.norder > 0) {
            sqlparams[sp++] = plan_offset;   /* OFFSET ? */
            sqlparams[sp++] = plan_limit;    /* FETCH NEXT ? */
        } else if (plan.has_limit && plan.norder > 0) {
            sqlparams[sp++] = plan_limit;    /* FETCH NEXT ? */
        }
        {
            VmsConnection* txn_cn = txn_current(tab->env);
            if (txn_cn) {
                /* R11: inside a transaction reads join the pinned
                 * identity (see own uncommitted writes; MARS) */
                cur->cur = vms_cursor_open_shared(txn_cn, sql, sqlparams, sp, &err);
            } else {
                cur->cur = vms_cursor_open_sql(lease, sql, sqlparams, sp, &err);
            }
        }
    } else {
        VmsConnection* txn_cn = txn_current(tab->env);
        if (txn_cn) {
            cur->cur = vms_cursor_open_shared(txn_cn, sql, NULL, 0, &err);
        } else {
            cur->cur = vms_cursor_open(lease, tab->schema, tab->table,
                                       tab->cols, tab->ncols, &err);
        }
    }
    /* R14: conservative read-only retry — a *fresh* cursor open may be
     * retried once on a transport/connect failure. This is safe because no
     * row has been exposed yet (the cursor does not exist) and the read is
     * idempotent. DML/COMMIT/ROLLBACK and any partially streamed result are
     * NEVER retried. */
    vms_pool_release(tab->env->pool, lease);
    lease = NULL;
    if (!cur->cur && !txn_current(tab->env)) {
        VmsErrClass cls = err.cls;
        if ((cls == VMS_ERR_TRANSPORT || cls == VMS_ERR_CONNECT ||
             cls == VMS_ERR_TIMEOUT) && !tab->is_query_source) {
            memset(&err, 0, sizeof(err));
            lease = vms_pool_acquire(tab->env->pool, &tab->env->profile, &err);
            if (lease) {
                if (have_plan) {
                    wchar_t rsql[2048];
                    int rnp = 0;
                    long long rparams[VMS_PLAN_MAX_ARGS];
                    int rsp = 0;
                    int a2;
                    if (vms_plan_build_sql(&plan, tab->schema, tab->table,
                                           tab->cols, tab->ncols,
                                           tab->spatial_wkt,
                                           rsql, 2048, &rnp)) {
                        if (plan.has_limit && plan.norder == 0 && !plan.has_offset)
                            rparams[rsp++] = plan_limit;
                        for (a2 = 0; a2 < plan.nterms; a2++) {
                            VmsPlanTerm* t = &plan.terms[a2];
                            if (t->op == VMS_OP_ISNULL || t->op == VMS_OP_ISNOTNULL) continue;
                            if (t->col < 0) continue;
                            rparams[rsp++] = term_params[a2];
                        }
                        if (plan.has_offset && plan.norder > 0) {
                            rparams[rsp++] = plan_offset;
                            rparams[rsp++] = plan_limit;
                        } else if (plan.has_limit && plan.norder > 0) {
                            rparams[rsp++] = plan_limit;
                        }
                        cur->cur = vms_cursor_open_sql(lease, rsql, rparams, rsp, &err);
                    }
                } else {
                    cur->cur = vms_cursor_open(lease, tab->schema, tab->table,
                                               tab->cols, tab->ncols, &err);
                }
                vms_pool_release(tab->env->pool, lease);
                lease = NULL;
            }
        }
    }
    if (!cur->cur) {
        vtab_set_error(&tab->base, &err);
        return SQLITE_ERROR;
    }
    cur->eof = 0;
    cur->rowid_counter = 0;
    /* build the vtab->projection column map: build_sql emits projected
     * columns in ascending vtab-index order */
    {
        int vcol, proj = 0;
        for (vcol = 0; vcol < tab->ncols && vcol < 512; vcol++) {
            int projected;
            if (have_plan && plan.used_mask != 0 && plan.used_mask != -1)
                projected = (vcol < 62) && (plan.used_mask & (1 << vcol));
            else
                projected = 1; /* full scan or all-columns plan */
            cur->col_map[vcol] = projected ? proj++ : -1;
        }
    }
    return vms_vtab_next(cursor); /* position on the first row */
}

static int vms_vtab_next(sqlite3_vtab_cursor* cursor)
{
    struct VmsVtabCursor* cur = (struct VmsVtabCursor*)cursor;

    /* R9 snapshot path */
    if (cur->snapshot_stmt) {
        int rc = sqlite3_step(cur->snapshot_stmt);
        if (rc == SQLITE_ROW) {
            cur->eof = 0;
            cur->rowid_counter++;
            return SQLITE_OK;
        }
        cur->eof = 1;
        sqlite3_finalize(cur->snapshot_stmt);
        cur->snapshot_stmt = NULL;
        return rc == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
    }

    if (!cur->cur) {
        cur->eof = 1;
        return SQLITE_OK;
    }
    {
        VmsError err;
        int r;
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
    }
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
    struct VmsVtab* tab = cur->tab;

    /* R9 snapshot path */
    if (cur->snapshot_stmt) {
        int c = cur->col_map[col];
        if (cur->eof || c < 0 || c >= sqlite3_column_count(cur->snapshot_stmt)) {
            sqlite3_result_null(ctx);
            return SQLITE_OK;
        }
        switch (sqlite3_column_type(cur->snapshot_stmt, c)) {
        case SQLITE_INTEGER:
            sqlite3_result_int64(ctx, sqlite3_column_int64(cur->snapshot_stmt, c));
            break;
        case SQLITE_FLOAT:
            sqlite3_result_double(ctx, sqlite3_column_double(cur->snapshot_stmt, c));
            break;
        case SQLITE_TEXT:
            sqlite3_result_text(ctx, (const char*)sqlite3_column_text(cur->snapshot_stmt, c),
                                sqlite3_column_bytes(cur->snapshot_stmt, c),
                                SQLITE_TRANSIENT);
            break;
        case SQLITE_BLOB:
            sqlite3_result_blob(ctx, sqlite3_column_blob(cur->snapshot_stmt, c),
                                sqlite3_column_bytes(cur->snapshot_stmt, c),
                                SQLITE_TRANSIENT);
            break;
        default:
            sqlite3_result_null(ctx);
            break;
        }
        return SQLITE_OK;
    }

    if (!cur->cur || cur->eof) {
        sqlite3_result_null(ctx);
        return SQLITE_OK;
    }
    {
        const VmsValue* v;
        if (col < 0 || col >= tab->ncols || cur->col_map[col] < 0) {
            sqlite3_result_null(ctx);
            return SQLITE_OK;
        }
        v = vms_cursor_value(cur->cur, cur->col_map[col]);
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
    }
    return SQLITE_OK;
}

static int vms_vtab_rowid(sqlite3_vtab_cursor* cursor, sqlite3_int64* pRowid)
{
    /* R10: for DELETE support the rowid must identify the remote row.
     * xUpdate receives this value in argv[0]; the write path re-reads the
     * row by the stable key, so we surface the first integer key column
     * when possible; otherwise the scan position (read-only usage). */
    struct VmsVtabCursor* cur = (struct VmsVtabCursor*)cursor;
    struct VmsVtab* tab = cur->tab;
    /* R10: stash all stable-key values of the current row so xUpdate can
     * rebuild the WHERE clause for GUID/composite/string keys. */
    tab->key_have = 0;
    if (cur->cur && tab->ncols > 0 && tab->rw_mode) {
        VmsDmlContext* d;
        VmsError derr;
        memset(&derr, 0, sizeof(derr));
        /* only reuse an existing DML context: creating one here would run
         * catalog queries on the scan connection while it is busy streaming
         * this very result set (driver error HY000 "connection busy") */
        if (tab->dml || tab->dml_txn) {
            d = tab->dml ? tab->dml : tab->dml_txn;
            int k;
            tab->key_have = 1;
            for (k = 0; k < d->key.part_count; k++) {
                int j, mapped = -1;
                for (j = 0; j < tab->ncols; j++) {
                    if (cur->col_map[j] >= 0 &&
                        !_stricmp(tab->cols[j].name, d->key.parts[k].name)) {
                        mapped = j; break;
                    }
                }
                if (mapped < 0) { tab->key_have = 0; break; }
                {
                    const VmsValue* v =
                        vms_cursor_value(cur->cur, cur->col_map[mapped]);
                    if (!v || v->type == VMS_VAL_NULL) { tab->key_have = 0; break; }
                    if (v->type == VMS_VAL_INT64)
                        _snprintf_s(tab->key_text[k], sizeof(tab->key_text[k]),
                                    _TRUNCATE, "%lld", v->i);
                    else if (v->type == VMS_VAL_FLOAT64)
                        _snprintf_s(tab->key_text[k], sizeof(tab->key_text[k]),
                                    _TRUNCATE, "%.17g", v->f);
                    else if (v->type == VMS_VAL_TEXT) {
                        size_t n = v->text_len < 159 ? v->text_len : 159;
                        memcpy(tab->key_text[k], v->text, n);
                        tab->key_text[k][n] = 0;
                    } else { tab->key_have = 0; break; }
                }
            }
        }
    }
    if (cur->cur && tab->ncols > 0) {
        int k, j;
        for (k = 0; k < 1; k++) {
            /* first key part that is an integer column (R5 key order) */
            for (j = 0; j < tab->ncols; j++) {
                const VmsValue* v;
                if (cur->col_map[j] < 0) continue;
                if (tab->cols[j].vtype != VMS_CT_INT64) continue;
                v = vms_cursor_value(cur->cur, cur->col_map[j]);
                if (v && v->type == VMS_VAL_INT64) { *pRowid = (sqlite3_int64)v->i; return SQLITE_OK; }
                break;
            }
            break;
        }
    }
    *pRowid = cur->rowid_counter;
    return SQLITE_OK;
}

/* ---------- R10 xUpdate (write path) ---------- */

/* per-xUpdate state for the value callback */
typedef struct UpdateVals {
    sqlite3_value** argv;   /* xUpdate argv */
    int argc;
} UpdateVals;

static const char* upd_value_get(void* user, int col, int* is_null)
{
    UpdateVals* u = (UpdateVals*)user;
    sqlite3_value* v;
    *is_null = 0;
    if (!u || col < 0 || col + 2 >= u->argc) { *is_null = 1; return NULL; }
    /* xUpdate argv layout: [0]=old rowid, [1]=new rowid, [2..nCol+1]=column
     * values (argc == nCol + 2) */
    v = u->argv[col + 2];
    if (sqlite3_value_type(v) == SQLITE_NULL) { *is_null = 1; return NULL; }
    return (const char*)sqlite3_value_text(v);
}

/* key value callback: reads from the xRowid stash (old key values) */
static const char* upd_key_stash_get(void* user, int col, int* is_null)
{
    struct VmsVtab* tab = (struct VmsVtab*)user;
    int part;
    *is_null = 0;
    if (!tab || !tab->key_have || col < 0 || col >= tab->ncols) {
        *is_null = 1;
        return NULL;
    }
    for (part = 0; part < VMS_META_MAX_KEY_PARTS; part++) {
        if (tab->key_part_col[part] == col)
            return tab->key_text[part];
    }
    *is_null = 1;
    return NULL;
}

/* lazily prepare the DML context (validates the stable key once). In an
 * explicit transaction the write goes through the pinned connection (all
 * statements join one remote transaction); otherwise it owns a dedicated
 * connection kept for the vtab's lifetime (autocommit writes). */
static int vtab_dml_ctx(struct VmsVtab* tab, VmsDmlContext** out, VmsError* err)
{
    VmsConnection* lease;
    VmsVtabEnv* env = tab->env;
    VmsConnection* txn_cn;

    /* R11: inside an explicit transaction route writes through the pin */
    EnterCriticalSection(&env->txn_cs);
    txn_cn = env->txn_cn;
    LeaveCriticalSection(&env->txn_cs);
    if (txn_cn) {
        if (tab->dml_txn && tab->dml_txn->cn == txn_cn) {
            *out = tab->dml_txn;
            return 1;
        }
        if (tab->dml_txn) HeapFree(GetProcessHeap(), 0, tab->dml_txn);
        tab->dml_txn = (VmsDmlContext*)HeapAlloc(GetProcessHeap(),
                                                 HEAP_ZERO_MEMORY,
                                                 sizeof(VmsDmlContext));
        if (!tab->dml_txn) {
            vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM dml ctx");
            return 0;
        }
        if (!vms_dml_init(tab->dml_txn, txn_cn, tab->schema, tab->table,
                          tab->cols, tab->ncols, err)) {
            HeapFree(GetProcessHeap(), 0, tab->dml_txn);
            tab->dml_txn = NULL;
            return 0;
        }
        /* also require the lazy BEGIN before the first DML in the txn */
        if (vms_txn_begin_lazy(txn_cn, err) != 0) return 0;
        *out = tab->dml_txn;
        return 1;
    }

    if (tab->dml) { *out = tab->dml; return 1; }
    if (!tab->rw_mode) return 0;
    lease = vms_pool_acquire(tab->env->pool, &tab->env->profile, err);
    if (!lease) return 0;
    {
        VmsDmlContext* d =
            (VmsDmlContext*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                      sizeof(VmsDmlContext));
        if (d && vms_dml_init(d, lease, tab->schema, tab->table,
                              tab->cols, tab->ncols, err)) {
            int k, j;
            tab->dml = d;
            for (k = 0; k < d->key.part_count; k++) {
                tab->key_part_col[k] = -1;
                for (j = 0; j < tab->ncols; j++) {
                    if (!_stricmp(tab->cols[j].name, d->key.parts[k].name)) {
                        tab->key_part_col[k] = j;
                        break;
                    }
                }
            }
            *out = d;
            /* the lease is owned by the context now: released at
             * xDisconnect via vms_conn_close (bypassing the pool, so the
             * write connection is never reused for reads) */
            return 1;
        }
        if (d) HeapFree(GetProcessHeap(), 0, d);
    }
    vms_pool_release(tab->env->pool, lease);
    return 0;
}

static int vms_vtab_update(sqlite3_vtab* vtab, int argc, sqlite3_value** argv,
                           sqlite3_int64* pRowid)
{
    struct VmsVtab* tab = (struct VmsVtab*)vtab;
    VmsDmlContext* d = NULL;
    VmsError err;
    UpdateVals uv;
    long long rows = 0;
    int r = 0;

    memset(&err, 0, sizeof(err));
    r = 0;

    if (!tab->rw_mode) {
        vtab->zErrMsg = sqlite3_mprintf(
            "virtualmssql: writes require mode=rw (this vtab is read-only)");
        return SQLITE_READONLY;
    }
    if (tab->is_query_source) {
        vtab->zErrMsg = sqlite3_mprintf(
            "virtualmssql: writes are not supported for source=query");
        return SQLITE_READONLY;
    }
    if (!vtab_dml_ctx(tab, &d, &err)) {
        vtab->zErrMsg = sqlite3_mprintf(
            "virtualmssql: no usable stable key on '%s.%s': writes rejected",
            tab->schema, tab->table);
        return SQLITE_ERROR;
    }

    uv.argv = argv;
    uv.argc = argc;

    if (argc == 1) {
        /* DELETE: argv[0] = the rowid surfaced by xRowid; the WHERE clause
         * is rebuilt from the stashed stable-key values (works for
         * GUID/composite/string keys too, not just integer rowids). */
        r = vms_dml_delete(d, upd_key_stash_get, tab, upd_value_get, &uv,
                           &rows, &err);
        if (r < 0) {
            vtab_set_error(vtab, &err);
            return SQLITE_ERROR;
        }
        if (rows == 0) {
            vtab->zErrMsg = sqlite3_mprintf(
                "virtualmssql: concurrent modification detected (0 rows deleted)");
            return SQLITE_BUSY_SNAPSHOT;
        }
        return SQLITE_OK;
    } else if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        /* INSERT: argv[1..ncols] = new values */
        r = vms_dml_insert(d, NULL, upd_value_get, &uv, &rows, &err);
    } else {
        /* UPDATE: argv[0] = old rowid, argv[1..ncols] = new values; the
         * WHERE clause uses the stashed *old* key values, never the new
         * ones (a key change must not retarget the row) */
        r = vms_dml_update(d, NULL, upd_key_stash_get, tab,
                           upd_value_get, &uv, &rows, &err);
    }

    if (r < 0) {
        vtab_set_error(vtab, &err);
        return SQLITE_ERROR;
    }
    /* NOTE: optimistic-lock conflict detection via SQLRowCount==0 proved
     * unreliable with this driver (0 is reported even for successful
     * INSERT); conflict detection moves to the R11 transaction layer using
     * the rowversion token. Here, success = no server error. */
    return SQLITE_OK;
}

/* ---------- R11 transactions ---------- */

/* current pinned transaction connection (or NULL); no locking side effects */
static VmsConnection* txn_current(VmsVtabEnv* env)
{
    VmsConnection* cn;
    EnterCriticalSection(&env->txn_cs);
    cn = env->txn_cn;
    LeaveCriticalSection(&env->txn_cs);
    return cn;
}

/* acquire (or return) the pinned transaction connection. The pin holds one
 * pool connection for the transaction's lifetime; the lazy BEGIN happens
 * on the first write, not here (xBegin precedes possibly read-only work).
 * The connection is opened with MARS enabled so that read cursors on the
 * same identity can stream concurrently with the writes (one canonical
 * SQL Server identity sees its own uncommitted data). */
static VmsConnection* txn_pin_shared(struct VmsVtab* tab, VmsError* err)
{
    VmsVtabEnv* env = tab->env;
    VmsConnection* cn;

    EnterCriticalSection(&env->txn_cs);
    if (env->txn_cn) {
        LeaveCriticalSection(&env->txn_cs);
        return env->txn_cn;
    }
    LeaveCriticalSection(&env->txn_cs);

    {
        /* the txn connection uses the standard profile connstr: MARS must
         * stay OFF because a MARS transaction is bound to its batch —
         * statements from different batches would not share one
         * transaction (server error 6401 on savepoint use). All access is
         * serialized by the connection's worker thread instead. */
        VmsProfile txn_profile = env->profile;
        wchar_t* connstr = NULL;
        size_t connstr_len = 0;
        VmsError verr;
        txn_profile.mars = 0;
        if (!vms_connstr_build(&txn_profile, &connstr, &connstr_len, &verr)) {
            *err = verr;
            return NULL;
        }
        cn = vms_conn_open(vms_pool_client(env->pool), connstr, &verr);
        vms_connstr_free(connstr);
        if (!cn) {
            *err = verr;
            return NULL;
        }
    }
    if (vms_txn_pin(cn, err) != 0) {
        vms_conn_close(cn);
        return NULL;
    }
    EnterCriticalSection(&env->txn_cs);
    if (env->txn_cn) {
        /* raced with another pin: drop the extra connection */
        LeaveCriticalSection(&env->txn_cs);
        vms_txn_rollback(cn, err);
        vms_conn_close(cn);
        EnterCriticalSection(&env->txn_cs);
        cn = env->txn_cn;
        LeaveCriticalSection(&env->txn_cs);
        return cn;
    }
    env->txn_cn = cn;
    env->txn_pinned = 1;
    LeaveCriticalSection(&env->txn_cs);
    return cn;
}

/* unpin and close the transaction connection (never returned to the pool:
 * it has a dedicated MARS connstr and may carry session state) */
static void txn_unpin(struct VmsVtab* tab)
{
    VmsVtabEnv* env = tab->env;
    VmsConnection* cn = NULL;
    EnterCriticalSection(&env->txn_cs);
    if (env->txn_cn) {
        cn = env->txn_cn;
        env->txn_cn = NULL;
        env->txn_pinned = 0;
        env->sv_count = 0;
    }
    LeaveCriticalSection(&env->txn_cs);
    if (cn) vms_conn_close(cn);
}

static int vms_vtab_begin(sqlite3_vtab* vtab)
{
    struct VmsVtab* tab = (struct VmsVtab*)vtab;
    VmsError err;
    memset(&err, 0, sizeof(err));
    if (!txn_pin_shared(tab, &err)) {
        vtab_set_error(vtab, &err);
        return SQLITE_ERROR;
    }
    return SQLITE_OK;
}

/* xSync: validation-only (R11). The remote transaction must exist and not
 * be doomed; no data is flushed here. */
static int vms_vtab_sync(sqlite3_vtab* vtab)
{
    struct VmsVtab* tab = (struct VmsVtab*)vtab;
    VmsVtabEnv* env = tab->env;
    VmsConnection* cn;

    EnterCriticalSection(&env->txn_cs);
    cn = env->txn_cn;
    LeaveCriticalSection(&env->txn_cs);
    if (!cn) return SQLITE_OK; /* no transaction: nothing to validate */

    if (!vms_txn_validate(cn)) {
        if (vms_txn_doomed(cn)) {
            vtab->zErrMsg = sqlite3_mprintf(
                "virtualmssql: transaction is uncommittable (XACT_STATE()=-1); "
                "rollback required");
            return SQLITE_ERROR;
        }
        vtab->zErrMsg = sqlite3_mprintf(
            "virtualmssql: transaction lost on the remote connection");
        return SQLITE_ERROR;
    }
    return SQLITE_OK;
}

/* xCommit: non-cancellable finalization */
static int vms_vtab_commit(sqlite3_vtab* vtab)
{
    struct VmsVtab* tab = (struct VmsVtab*)vtab;
    VmsVtabEnv* env = tab->env;
    VmsConnection* cn;
    VmsTxnResult res;
    VmsError err;
    memset(&err, 0, sizeof(err));

    EnterCriticalSection(&env->txn_cs);
    cn = env->txn_cn;
    LeaveCriticalSection(&env->txn_cs);
    if (!cn) return SQLITE_OK; /* nothing joined the transaction */

    res = vms_txn_commit(cn, &err);
    if (res == VMS_TXN_OK) {
        txn_unpin(tab);
        return SQLITE_OK;
    }
    if (res == VMS_TXN_BUSY) {
        txn_unpin(tab);
        vtab->zErrMsg = sqlite3_mprintf(
            "virtualmssql: commit timed out against a conflicting operation");
        return SQLITE_BUSY;
    }
    /* VMS_TXN_UNKNOWN: outcome unknowable (quarantined connection) */
    txn_unpin(tab);
    vtab->zErrMsg = sqlite3_mprintf(
        "virtualmssql: commit outcome unknown; the connection was "
        "quarantined and the data state must be verified manually");
    return SQLITE_ERROR;
}

/* xRollback: non-cancellable finalization (rollback has no unknown-outcome
 * ambiguity: server either rolls back or the connection is dead) */
static int vms_vtab_rollback(sqlite3_vtab* vtab)
{
    struct VmsVtab* tab = (struct VmsVtab*)vtab;
    VmsVtabEnv* env = tab->env;
    VmsConnection* cn;
    VmsTxnResult res;
    VmsError err;
    memset(&err, 0, sizeof(err));

    EnterCriticalSection(&env->txn_cs);
    cn = env->txn_cn;
    LeaveCriticalSection(&env->txn_cs);
    if (!cn) return SQLITE_OK;

    res = vms_txn_rollback(cn, &err);
    txn_unpin(tab);
    if (res == VMS_TXN_OK) return SQLITE_OK;
    if (res == VMS_TXN_BUSY) {
        vtab->zErrMsg = sqlite3_mprintf(
            "virtualmssql: rollback timed out against a conflicting operation");
        return SQLITE_BUSY;
    }
    vtab->zErrMsg = sqlite3_mprintf(
        "virtualmssql: rollback failed; connection quarantined");
    return SQLITE_ERROR;
}

/* savepoint names: vms_sv_<n> (validated identifier, never user input) */
static int vms_vtab_savepoint(sqlite3_vtab* vtab, int n)
{
    struct VmsVtab* tab = (struct VmsVtab*)vtab;
    VmsVtabEnv* env = tab->env;
    VmsConnection* cn;
    VmsError err;
    char name[48];

    memset(&err, 0, sizeof(err));
    cn = txn_pin_shared(tab, &err);
    if (!cn) {
        vtab_set_error(vtab, &err);
        return SQLITE_ERROR;
    }
    /* savepoint before the first write implies the lazy BEGIN */
    if (vms_txn_begin_lazy(cn, &err) != 0) {
        vtab_set_error(vtab, &err);
        return SQLITE_ERROR;
    }
    _snprintf_s(name, sizeof(name), _TRUNCATE, "vms_sv_%d", n);
    if (vms_txn_savepoint(cn, name, 0, &err) != 0) {
        vtab_set_error(vtab, &err);
        return SQLITE_ERROR;
    }
    EnterCriticalSection(&env->txn_cs);
    if (env->sv_count < VMS_TXN_MAX_SAVEPOINTS) {
        strncpy_s(env->sv_names[env->sv_count], sizeof(env->sv_names[0]),
                  name, _TRUNCATE);
        env->sv_count++;
    }
    LeaveCriticalSection(&env->txn_cs);
    return SQLITE_OK;
}

static int vms_vtab_release(sqlite3_vtab* vtab, int n)
{
    struct VmsVtab* tab = (struct VmsVtab*)vtab;
    VmsVtabEnv* env = tab->env;
    /* local bookkeeping only: the remote SAVE stays valid for the remaining
     * nesting level; SQLite guarantees release of the topmost savepoint */
    EnterCriticalSection(&env->txn_cs);
    if (env->sv_count > 0) env->sv_count--;
    LeaveCriticalSection(&env->txn_cs);
    return SQLITE_OK;
}

static int vms_vtab_rollback_to(sqlite3_vtab* vtab, int n)
{
    struct VmsVtab* tab = (struct VmsVtab*)vtab;
    VmsVtabEnv* env = tab->env;
    VmsConnection* cn;
    VmsError err;
    char name[48];
    int keep;

    memset(&err, 0, sizeof(err));
    EnterCriticalSection(&env->txn_cs);
    cn = env->txn_cn;
    LeaveCriticalSection(&env->txn_cs);
    if (!cn) return SQLITE_OK; /* nothing to roll back to */

    _snprintf_s(name, sizeof(name), _TRUNCATE, "vms_sv_%d", n);
    if (vms_txn_savepoint(cn, name, 1, &err) != 0) {
        vtab_set_error(vtab, &err);
        return SQLITE_ERROR;
    }
    /* drop bookkeeping above the restored savepoint (n stays valid) */
    EnterCriticalSection(&env->txn_cs);
    keep = n + 1;
    if (env->sv_count > keep) env->sv_count = keep;
    LeaveCriticalSection(&env->txn_cs);
    return SQLITE_OK;
}

/* ---------- R13 shadow / integrity ---------- */

/* xShadowName: reject any attempt to address a shadow-named object of a
 * virtualmssql table directly (e.g. t12_vms_schema) — the shadow storage
 * is private to the module. */
static int vms_vtab_shadow_name(const char* name)
{
    const char* s1 = "_vms_schema";
    const char* s2 = "_vms_metadata";
    size_t n = strlen(name);
    size_t l1 = strlen(s1), l2 = strlen(s2);
    if (n > l1 && !strcmp(name + n - l1, s1)) return 1;
    if (n > l2 && !strcmp(name + n - l2, s2)) return 1;
    return 0;
}

/* xIntegrity: validation-only self-check over the module state for this
 * vtab. Performs a structural audit WITHOUT a remote connection: the
 * declared column count must be within bounds, every column must have a
 * valid registry type and a non-empty name, and cached-mode tables must
 * carry a non-zero schema fingerprint. Returns SQLITE_OK or an error. */
static int vms_vtab_integrity(sqlite3_vtab* vtab, const char* zSchema,
                              const char* zName, int isQuick, char** pzErr)
{
    struct VmsVtab* tab = (struct VmsVtab*)vtab;
    int i;

    (void)zSchema;
    (void)zName;
    (void)isQuick;
    if (tab->ncols < 1 || tab->ncols > 512) {
        *pzErr = sqlite3_mprintf("virtualmssql: integrity: ncols=%d out of range",
                                 tab->ncols);
        return SQLITE_ERROR;
    }
    for (i = 0; i < tab->ncols; i++) {
        if (!tab->cols[i].name[0]) {
            *pzErr = sqlite3_mprintf(
                "virtualmssql: integrity: column %d has an empty name", i);
            return SQLITE_ERROR;
        }
        if (tab->cols[i].vtype < VMS_CT_INT64 ||
            tab->cols[i].vtype > VMS_CT_SPATIAL) {
            *pzErr = sqlite3_mprintf(
                "virtualmssql: integrity: column '%s' has an invalid type code %d",
                tab->cols[i].name, (int)tab->cols[i].vtype);
            return SQLITE_ERROR;
        }
        if (tab->cols[i].vtype == VMS_CT_UNSUPPORTED) {
            *pzErr = sqlite3_mprintf(
                "virtualmssql: integrity: column '%s' is UNSUPPORTED_TYPE",
                tab->cols[i].name);
            return SQLITE_ERROR;
        }
    }
    if (tab->metadata_cached && !tab->is_query_source && tab->schema_fp == 0) {
        *pzErr = sqlite3_mprintf(
            "virtualmssql: integrity: cached-mode vtab carries no schema fingerprint");
        return SQLITE_ERROR;
    }
    return SQLITE_OK;
}

/* ---------- module struct ---------- */

static sqlite3_module vms_module = {
    3,                 /* iVersion (xShadowName + xIntegrity required) */
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
    vms_vtab_update, /* xUpdate (R10; gated by mode=rw + stable key) */
    vms_vtab_begin,       /* xBegin */
    vms_vtab_sync,        /* xSync (validation-only) */
    vms_vtab_commit,      /* xCommit */
    vms_vtab_rollback,    /* xRollback */
    0,                    /* xFindFunction */
    0,                    /* xRename */
    vms_vtab_savepoint,   /* xSavepoint */
    vms_vtab_release,     /* xRelease */
    vms_vtab_rollback_to, /* xRollbackTo */
    0,                    /* xShadowName (R13: declared, checked in connect) */
    vms_vtab_integrity    /* xIntegrity (R13: offline self-check) */
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
    InitializeCriticalSection(&env->txn_cs);
    env->txn_cs_init = 1;
    InitializeCriticalSection(&env->cache_cs);
    env->cache_cs_init = 1;
    env->mdcache = vms_meta_cache_open(0);
    env->pool = vms_pool_create(4);
    if (!env->pool) {
        vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM vtab pool");
        if (env->mdcache) vms_meta_cache_close(env->mdcache);
        DeleteCriticalSection(&env->cache_cs);
        DeleteCriticalSection(&env->txn_cs);
        HeapFree(GetProcessHeap(), 0, env);
        return NULL;
    }
    return env;
}

void vms_vtab_env_destroy(VmsVtabEnv* env)
{
    if (!env) return;
    if (env->txn_cn) {
        VmsError err;
        memset(&err, 0, sizeof(err));
        vms_txn_rollback(env->txn_cn, &err);
        env->txn_cn = NULL;
    }
    if (env->pool) vms_pool_destroy(env->pool);
    if (env->mdcache) vms_meta_cache_close(env->mdcache);
    if (env->cache_cs_init) DeleteCriticalSection(&env->cache_cs);
    if (env->txn_cs_init) DeleteCriticalSection(&env->txn_cs);
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


