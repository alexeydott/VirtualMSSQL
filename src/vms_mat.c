/* vms_mat.c — query materialization (R9).
 *
 * Build: streams rows from the remote cursor into a private SQLite db
 * (batched transactions), then creates query_indexes, then — atomically —
 * flips the published flag. Any failure/cancel leaves FAILED and the
 * partial snapshot is dropped (readers never see it, since publishing is
 * a single state store swap that happens only on success).
 *
 * NOTE: compiled with SQLITE_CORE (set in CMakeLists): the materializer
 * uses the linked host sqlite3 directly for the private snapshot db —
 * no api-thunk involvement, so this TU works identically in the DLL and
 * in test binaries that link the host import library. */
#include <sqlite3.h>
#include "vms_mat.h"
#include <windows.h>
#include <string.h>
#include <stdio.h>

#define VMS_MAT_BATCH 2000
#define VMS_MAT_TABLE "vms_snapshot"

struct VmsMat {
    VmsMatMode mode;
    VmsMatState state;
    sqlite3* db;
    long long max_rows;
    long long max_bytes;
    long long rows;
    volatile LONG cancelled;
    char db_path[512];
    /* atomic publish bookkeeping */
    CRITICAL_SECTION cs;
};

int vms_mat_mode_parse(const char* s)
{
    if (!s || !s[0]) return -1;
    if (!_stricmp(s, "off")) return VMS_MAT_OFF;
    if (!_stricmp(s, "memory")) return VMS_MAT_MEMORY;
    if (!_stricmp(s, "temp")) return VMS_MAT_TEMP;
    return -1;
}

VmsMat* vms_mat_create(VmsMatMode mode, long long max_rows, long long max_bytes)
{
    VmsMat* m;
    if (mode == VMS_MAT_OFF) return NULL;
    m = (VmsMat*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(VmsMat));
    if (!m) return NULL;
    InitializeCriticalSection(&m->cs);
    m->mode = mode;
    m->state = VMS_MAT_BUILDING;
    m->max_rows = max_rows > 0 ? max_rows : 10000000;
    m->max_bytes = max_bytes > 0 ? max_bytes : 1073741824LL; /* 1 GiB */
    if (mode == VMS_MAT_MEMORY) {
        strcpy_s(m->db_path, sizeof(m->db_path), ":memory:");
    } else {
        /* private temp file in the user temp directory */
        char tmpdir[400];
        char name[96];
        GetTempPathA(sizeof(tmpdir), tmpdir);
        _snprintf_s(name, sizeof(name), _TRUNCATE,
                    "vms_mat_%lu_%u.db", (unsigned long)GetCurrentProcessId(),
                    (unsigned)GetTickCount());
        _snprintf_s(m->db_path, sizeof(m->db_path), _TRUNCATE,
                    "%s%s", tmpdir, name);
        DeleteFileA(m->db_path); /* stale leftovers */
    }
    return m;
}

void vms_mat_destroy(VmsMat* m)
{
    if (!m) return;
    if (m->db) {
        sqlite3_close(m->db);
        m->db = NULL;
    }
    if (m->mode == VMS_MAT_TEMP && m->db_path[0] && strcmp(m->db_path, ":memory:") != 0) {
        DeleteFileA(m->db_path);
    }
    DeleteCriticalSection(&m->cs);
    HeapFree(GetProcessHeap(), 0, m);
}

VmsMatState vms_mat_state(const VmsMat* m)
{
    return m ? m->state : VMS_MAT_FAILED;
}

void* vms_mat_db(VmsMat* m)
{
    if (!m || m->state != VMS_MAT_PUBLISHED) return NULL;
    return m->db;
}

const char* vms_mat_table_name(void)
{
    return VMS_MAT_TABLE;
}

long long vms_mat_row_count(const VmsMat* m)
{
    return m ? m->rows : 0;
}

void vms_mat_cancel(VmsMat* m)
{
    if (m) InterlockedExchange(&m->cancelled, 1);
}

/* bind one VmsValue to a statement parameter (columns only; text/blob
 * use SQLITE_TRANSIENT so the private db owns its copy) */
static int bind_value(sqlite3_stmt* st, int idx, const VmsValue* v)
{
    switch (v->type) {
    case VMS_VAL_NULL:
        return sqlite3_bind_null(st, idx) == SQLITE_OK;
    case VMS_VAL_INT64:
        return sqlite3_bind_int64(st, idx, (sqlite3_int64)v->i) == SQLITE_OK;
    case VMS_VAL_FLOAT64:
        return sqlite3_bind_double(st, idx, v->f) == SQLITE_OK;
    case VMS_VAL_TEXT:
        return sqlite3_bind_text(st, idx, v->text, (int)v->text_len,
                                 SQLITE_TRANSIENT) == SQLITE_OK;
    case VMS_VAL_BLOB:
        return sqlite3_bind_blob(st, idx, v->blob, (int)v->blob_len,
                                 SQLITE_TRANSIENT) == SQLITE_OK;
    default:
        return 0;
    }
}

static int fail(VmsMat* m, VmsError* err, const char* msg)
{
    EnterCriticalSection(&m->cs);
    m->state = VMS_MAT_FAILED;
    LeaveCriticalSection(&m->cs);
    vms_error_set(err, VMS_ERR_INTERNAL, NULL, 0, "materialization: %s", msg);
    if (m->db) {
        sqlite3_close(m->db);
        m->db = NULL;
    }
    if (m->mode == VMS_MAT_TEMP && m->db_path[0] && strcmp(m->db_path, ":memory:") != 0) {
        DeleteFileA(m->db_path);
    }
    return 0;
}

int vms_mat_build(VmsMat* m, VmsConnection* cn, const VmsQuerySource* src,
                  VmsError* err)
{
    wchar_t cursor_sql[34816];
    VmsCursor* cur = NULL;
    sqlite3_stmt* insert = NULL;
    char ddl[8192];
    size_t dlen = 0;
    char create_index[512];
    int batch = 0;
    int i;
    int rc;

    vms_error_ok(err);
    if (!m || !cn || !src || m->state != VMS_MAT_BUILDING || m->mode == VMS_MAT_OFF) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "materialization: bad args/state");
        return 0;
    }

    /* open the private db */
    if (sqlite3_open(m->db_path, &m->db) != SQLITE_OK) {
        return fail(m, err, "cannot open private db");
    }
    sqlite3_extended_result_codes(m->db, 1);

    /* snapshot table DDL from the described shape */
    memcpy(ddl, "CREATE TABLE " VMS_MAT_TABLE "(", 27);
    dlen = 26;
    for (i = 0; i < src->ncols; i++) {
        const char* affinity;
        char col[280];
        switch (src->cols[i].vtype) {
        case VMS_CT_INT64:  affinity = "INTEGER"; break;
        case VMS_CT_FLOAT64: affinity = "REAL"; break;
        case VMS_CT_BLOB:   affinity = "BLOB"; break;
        default:            affinity = "TEXT"; break;
        }
        _snprintf_s(col, sizeof(col), _TRUNCATE, "%s\"%s\" %s",
                    i ? "," : "", src->cols[i].name, affinity);
        if (dlen + strlen(col) + 2 >= sizeof(ddl)) {
            return fail(m, err, "schema too wide");
        }
        memcpy(ddl + dlen, col, strlen(col) + 1);
        dlen += strlen(col);
    }
    memcpy(ddl + dlen, ")", 2);
    if (sqlite3_exec(m->db, ddl, NULL, NULL, NULL) != SQLITE_OK) {
        return fail(m, err, "snapshot DDL failed");
    }

    if (sqlite3_exec(m->db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK) {
        return fail(m, err, "cannot begin snapshot transaction");
    }
    {
        char ins[8192];
        int w = _snprintf_s(ins, sizeof(ins), _TRUNCATE,
                            "INSERT INTO " VMS_MAT_TABLE " VALUES(");
        for (i = 0; i < src->ncols; i++) {
            int n = _snprintf_s(ins + w, sizeof(ins) - w, _TRUNCATE,
                                "%s?", i ? "," : "");
            if (n < 0) return fail(m, err, "insert stmt too wide");
            w += n;
        }
        if (_snprintf_s(ins + w, sizeof(ins) - w, _TRUNCATE, ")") < 0 ||
            sqlite3_prepare_v2(m->db, ins, -1, &insert, NULL) != SQLITE_OK) {
            return fail(m, err, "insert prepare failed");
        }
    }

    /* stream the rows */
    if (!vms_query_source_get_sql(src, cursor_sql, 34816)) {
        sqlite3_finalize(insert);
        return fail(m, err, "query copy failed");
    }
    cur = vms_cursor_open_sql(cn, cursor_sql, NULL, 0, err);
    if (!cur) {
        sqlite3_finalize(insert);
        return fail(m, err, err->message);
    }
    for (;;) {
        int r = vms_cursor_fetch(cur, err);
        if (r < 0) {
            vms_cursor_close(cur);
            sqlite3_finalize(insert);
            return fail(m, err, "remote scan failed");
        }
        if (r == 0) break;
        if (InterlockedCompareExchange(&m->cancelled, 0, 0)) {
            vms_cursor_close(cur);
            sqlite3_finalize(insert);
            return fail(m, err, "cancelled");
        }
        if (m->rows >= m->max_rows) {
            vms_cursor_close(cur);
            sqlite3_finalize(insert);
            return fail(m, err, "row limit exceeded");
        }
        for (i = 0; i < src->ncols; i++) {
            const VmsValue* v = vms_cursor_value(cur, i);
            if (!bind_value(insert, i + 1, v ? v : &(VmsValue){VMS_VAL_NULL})) {
                vms_cursor_close(cur);
                sqlite3_finalize(insert);
                return fail(m, err, "bind failed");
            }
        }
        rc = sqlite3_step(insert);
        if (rc != SQLITE_DONE) {
            vms_cursor_close(cur);
            sqlite3_finalize(insert);
            return fail(m, err, "insert step failed");
        }
        sqlite3_reset(insert);
        m->rows++;
        if (++batch >= VMS_MAT_BATCH) {
            batch = 0;
            if (sqlite3_exec(m->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK ||
                sqlite3_exec(m->db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK) {
                vms_cursor_close(cur);
                sqlite3_finalize(insert);
                return fail(m, err, "commit failed");
            }
        }
    }
    vms_cursor_close(cur);
    cur = NULL;
    sqlite3_finalize(insert);
    insert = NULL;

    if (sqlite3_exec(m->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        return fail(m, err, "final commit failed");
    }

    /* query_indexes: an index on every integer column helps local filters */
    for (i = 0; i < src->ncols; i++) {
        if (src->cols[i].vtype != VMS_CT_INT64) continue;
        _snprintf_s(create_index, sizeof(create_index), _TRUNCATE,
                    "CREATE INDEX " VMS_MAT_TABLE "_i%d ON " VMS_MAT_TABLE "(\"%s\")",
                    i, src->cols[i].name);
        if (sqlite3_exec(m->db, create_index, NULL, NULL, NULL) != SQLITE_OK) {
            return fail(m, err, "index creation failed");
        }
    }

    /* READY -> PUBLISHED: the atomic switch. Readers only ever observe the
     * published state, and the state store swap is a single critical-
     * section assignment. */
    EnterCriticalSection(&m->cs);
    m->state = VMS_MAT_PUBLISHED; /* READY step elided: readers check only PUBLISHED/FAILED */
    LeaveCriticalSection(&m->cs);
    return 1;
}

