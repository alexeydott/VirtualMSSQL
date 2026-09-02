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
#include "vms_plan.h"
#include "vms_query_source.h"
#include "vms_mat.h"
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
    /* R8: query source mode */
    int is_query_source;
    VmsQuerySource qsrc;
    char query_spec[32768];
    /* R9: materialization (query sources only) */
    VmsMatMode mat_mode;
    VmsMat* mat;           /* published snapshot; guarded by SQLite core */
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
    int mat_mode_parsed = VMS_MAT_OFF;

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

    /* discover the shape through a pool lease */
    {
        VmsConnection* probe = vms_pool_acquire(g_env->pool, &g_env->profile, &err);
        if (!probe) {
            *pzErr = sqlite3_mprintf("virtualmssql: probe lease failed: %s", err.message);
            sqlite3_free(tab);
            return SQLITE_ERROR;
        }
        if (is_query) {
            /* R8: validate + describe via sp_describe_first_result_set */
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
    }
    tab->ncols = is_query ? tab->qsrc.ncols : cols.count;
    if (tab->ncols < 1 || tab->ncols > 512) {
        *pzErr = sqlite3_mprintf("virtualmssql: object has %d columns (unsupported)",
                                 tab->ncols);
        sqlite3_free(tab);
        return SQLITE_ERROR;
    }
    if (is_query) {
        tab->mat_mode = (VmsMatMode)mat_mode_parsed;
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
    if (tab->is_query_source) vms_query_source_free(&tab->qsrc);
    if (tab->mat) vms_mat_destroy(tab->mat);
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
                                tab->cols, tab->ncols, sql, 2048, &np)) {
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
        cur->cur = vms_cursor_open_sql(lease, sql, sqlparams, sp, &err);
    } else {
        cur->cur = vms_cursor_open(lease, tab->schema, tab->table,
                                   tab->cols, tab->ncols, &err);
    }
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
