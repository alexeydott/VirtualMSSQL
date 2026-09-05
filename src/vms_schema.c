/* vms_schema.c — remote schema inspection as table-valued functions (R18).
 *
 * Four direct-only, eponymous table functions mirror the PRAGMA table
 * functions for the remote SQL Server schema:
 *
 *   virtualmssql_tables(connection, schema)
 *   virtualmssql_table_info(connection, schema, table)
 *   virtualmssql_index_list(connection, schema, table)
 *   virtualmssql_index_info(connection, schema, table, index)
 *
 * The first hidden argument is a comma-separated runtime connection
 * specification in the virtualmssql_profile grammar. auth=sql resolves
 * cred=<key> through the registered credential provider. Identifier
 * arguments are validated and inlined as N''-quoted literals.
 *
 * table_info follows PRAGMA table_info; index_list/index_info follow
 * PRAGMA index_list/index_xinfo. Direct-only: invoke from top-level SQL. */
#include "vms_vtab.h"
#include "vms_meta.h"
#include "vms_client.h"
#include "vms_error.h"
#include "sqlite3ext.h"
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef SQLITE_CORE
SQLITE_EXTENSION_INIT3
#endif

#define VMS_SI_MAX_HIDDEN 4

typedef struct VmsSiVtab {
    sqlite3_vtab base;
    int kind;
} VmsSiVtab;

typedef struct VmsSiCursor {
    sqlite3_vtab_cursor base;
    VmsCursor* cur;
    int eof;
} VmsSiCursor;

enum { SI_TABLES = 0, SI_TABLE_INFO = 1, SI_INDEX_LIST = 2, SI_INDEX_INFO = 3 };

/* hidden column count per kind */
static const int k_si_hidden[] = { 2, 3, 3, 4 };

static const char* k_si_decl[] = {
    /* tables */
    "CREATE TABLE x("
    " connection TEXT HIDDEN, schema_name TEXT HIDDEN,"
    " table_schema TEXT, table_name TEXT, table_type TEXT)",
    /* table_info */
    "CREATE TABLE x("
    " connection TEXT HIDDEN, schema_name TEXT HIDDEN, table_name TEXT HIDDEN,"
    " cid INTEGER, name TEXT, type TEXT, \"notnull\" INTEGER, dflt_value TEXT,"
    " pk INTEGER, ordinal INTEGER, max_length INTEGER, precision INTEGER,"
    " scale INTEGER, is_nullable INTEGER, is_identity INTEGER,"
    " is_computed INTEGER, hidden_flags INTEGER)",
    /* index_list */
    "CREATE TABLE x("
    " connection TEXT HIDDEN, schema_name TEXT HIDDEN, table_name TEXT HIDDEN,"
    " seq INTEGER, name TEXT, \"unique\" INTEGER, origin TEXT, partial INTEGER,"
    " is_disabled INTEGER, column_count INTEGER)",
    /* index_info */
    "CREATE TABLE x("
    " connection TEXT HIDDEN, schema_name TEXT HIDDEN, table_name TEXT HIDDEN,"
    " index_name TEXT HIDDEN,"
    " seqno INTEGER, cid INTEGER, name TEXT, \"desc\" INTEGER,"
    " collation TEXT, key INTEGER, is_primary_key INTEGER, is_unique INTEGER,"
    " is_disabled INTEGER, key_ordinal INTEGER, is_nullable INTEGER)",
};

/* build the catalog query with N''-quoted validated identifiers */
static int si_build_sql(int kind, const wchar_t* schema, const wchar_t* table,
                        const wchar_t* index, wchar_t* sql, size_t cap)
{
    switch (kind) {
    case SI_TABLES:
        if (!schema[0]) return 0;
        return swprintf_s(sql, cap,
            L"SELECT s.name, o.name,"
            L" CASE o.type WHEN N'U' THEN N'table' WHEN N'V' THEN N'view' END"
            L" FROM sys.objects o JOIN sys.schemas s ON s.schema_id = o.schema_id"
            L" WHERE o.type IN (N'U', N'V') AND s.name = N'%s'"
            L" ORDER BY s.name, o.name", schema) > 0;
    case SI_TABLE_INFO:
        if (!schema[0] || !table[0]) return 0;
        return swprintf_s(sql, cap,
            L"SELECT c.column_id - 1, c.name, t.name, c.is_nullable,"
            L" OBJECT_DEFINITION(c.default_object_id),"
            L" COALESCE(pkic.key_ordinal, 0),"
            L" CASE WHEN c.is_computed = 1 THEN 2 ELSE 0 END,"
            L" c.column_id, c.max_length, c.precision, c.scale, c.is_nullable,"
            L" c.is_identity, c.is_computed"
            L" FROM sys.columns c"
            L" JOIN sys.types t ON t.user_type_id = c.user_type_id"
            L" JOIN sys.objects o ON o.object_id = c.object_id"
            L" JOIN sys.schemas s ON s.schema_id = o.schema_id"
            L" LEFT JOIN sys.index_columns pkic"
            L"   ON pkic.object_id = c.object_id AND pkic.column_id = c.column_id"
            L" LEFT JOIN sys.indexes pi"
            L"   ON pi.object_id = pkic.object_id AND pi.index_id = pkic.index_id"
            L"  AND pi.is_primary_key = 1"
            L" WHERE s.name = N'%s' AND o.name = N'%s' ORDER BY c.column_id",
            schema, table) > 0;
    case SI_INDEX_LIST:
        if (!schema[0] || !table[0]) return 0;
        return swprintf_s(sql, cap,
            L"SELECT ROW_NUMBER() OVER (ORDER BY i.index_id), i.name,"
            L" i.is_unique,"
            L" CASE WHEN i.is_primary_key = 1 THEN N'pk'"
            L"  WHEN i.is_unique = 1 THEN N'u' ELSE N'c' END,"
            L" CASE WHEN i.has_filter = 1 THEN 1 ELSE 0 END, i.is_disabled,"
            L" (SELECT COUNT(*) FROM sys.index_columns x"
            L"  WHERE x.object_id = i.object_id AND x.index_id = i.index_id)"
            L" FROM sys.indexes i"
            L" JOIN sys.objects o ON o.object_id = i.object_id"
            L" JOIN sys.schemas s ON s.schema_id = o.schema_id"
            L" WHERE s.name = N'%s' AND o.name = N'%s' AND i.index_id > 0"
            L" ORDER BY i.index_id", schema, table) > 0;
    case SI_INDEX_INFO:
        if (!schema[0] || !table[0] || !index[0]) return 0;
        return swprintf_s(sql, cap,
            L"SELECT ic.key_ordinal, ic.index_column_id, c.name,"
            L" ic.is_descending_key, N'B',"
            L" CASE WHEN ic.is_included_column = 1 THEN 0 ELSE 1 END,"
            L" i.is_primary_key, i.is_unique, i.is_disabled, ic.key_ordinal,"
            L" c.is_nullable"
            L" FROM sys.index_columns ic"
            L" JOIN sys.columns c ON c.object_id = ic.object_id"
            L"  AND c.column_id = ic.column_id"
            L" JOIN sys.indexes i ON i.object_id = ic.object_id"
            L"  AND i.index_id = ic.index_id"
            L" JOIN sys.objects o ON o.object_id = i.object_id"
            L" JOIN sys.schemas s ON s.schema_id = o.schema_id"
            L" WHERE s.name = N'%s' AND o.name = N'%s' AND i.name = N'%s'"
            L" ORDER BY ic.key_ordinal", schema, table, index) > 0;
    default:
        return 0;
    }
}

/* splice a replacement into the first occurrence of marker */
static void si_splice(wchar_t* sql, size_t cap, const wchar_t* marker,
                      const wchar_t* replacement)
{
    wchar_t* p = wcsstr(sql, marker);
    if (!p) return;
    {
        size_t mlen = wcslen(marker);
        size_t rlen = wcslen(replacement);
        size_t tail_len = wcslen(p + mlen);
        size_t off = (size_t)(p - sql);
        if (off + rlen + tail_len + 1 > cap) return;
        if (rlen != mlen)
            memmove(p + rlen, p + mlen, (tail_len + 1) * sizeof(wchar_t));
        memcpy(p, replacement, rlen * sizeof(wchar_t));
    }
}

static int si_connect(sqlite3* db, void* pAux, int argc,
                      const char* const* argv, sqlite3_vtab** ppVtab,
                      char** pzErr)
{
    int kind = (int)(intptr_t)pAux;
    VmsSiVtab* v = (VmsSiVtab*)sqlite3_malloc(sizeof(VmsSiVtab));
    if (!v) return SQLITE_NOMEM;
    memset(v, 0, sizeof(*v));
    v->kind = kind;
    if (sqlite3_declare_vtab(db, k_si_decl[kind]) != SQLITE_OK) {
        sqlite3_free(v);
        return SQLITE_ERROR;
    }
    *ppVtab = &v->base;
    (void)argc; (void)argv; (void)pzErr;
    return SQLITE_OK;
}

static int si_disconnect(sqlite3_vtab* vtab)
{
    sqlite3_free(vtab);
    return SQLITE_OK;
}

static int si_best_index(sqlite3_vtab* vtab, sqlite3_index_info* info)
{
    VmsSiVtab* v = (VmsSiVtab*)vtab;
    int i;
    int nh = k_si_hidden[v->kind];
    char order[VMS_SI_MAX_HIDDEN];
    int next = 1;

    memset(order, 0, sizeof(order));
    for (i = 0; i < info->nConstraint; i++) {
        const struct sqlite3_index_constraint* c = &info->aConstraint[i];
        if (!c->usable || c->op != SQLITE_INDEX_CONSTRAINT_EQ) continue;
        /* only consume constraints on HIDDEN columns (function args) */
        if (c->iColumn < 0 || c->iColumn >= nh) continue;
        if (order[c->iColumn]) continue;
        order[c->iColumn] = (char)next;
        info->aConstraintUsage[i].argvIndex = next;
        info->aConstraintUsage[i].omit = 1;
        next++;
    }
    info->idxStr = (char*)sqlite3_malloc(VMS_SI_MAX_HIDDEN);
    if (!info->idxStr) return SQLITE_NOMEM;
    memcpy(info->idxStr, order, VMS_SI_MAX_HIDDEN);
    info->needToFreeIdxStr = 1;
    info->idxNum = (order[0] != 0) ? 1 : 0;
    info->estimatedCost = (order[0] != 0) ? 20.0 : 1e9;
    info->estimatedRows = (order[0] != 0) ? 200 : 1000000;
    return SQLITE_OK;
}

static int si_arg_w(sqlite3_value* v, wchar_t* out, size_t cap)
{
    const unsigned char* t;
    if (sqlite3_value_type(v) != SQLITE_TEXT) return 0;
    t = sqlite3_value_text(v);
    if (!t || !t[0]) return 0;
    return MultiByteToWideChar(CP_UTF8, 0, (const char*)t, -1, out, (int)cap) > 0;
}

static void si_set_error(sqlite3_vtab* vtab, const VmsError* err)
{
    if (vtab && err && err->cls != VMS_OK)
        vtab->zErrMsg = sqlite3_mprintf("virtualmssql: %s (%s)",
                                        err->message, err->sqlstate);
}

/* process-lifetime ODBC environment for inspection connections */
static VmsClient* si_client(void)
{
    static VmsClient* c = NULL;
    static LONG init_done = 0;
    if (!InterlockedCompareExchangePointer((volatile PVOID*)&c, NULL, NULL)) {
        if (InterlockedCompareExchange(&init_done, 1, 0) == 0) {
            VmsError err;
            VmsClient* made = vms_client_init(&err);
            if (made)
                InterlockedExchangePointer((volatile PVOID*)&c, made);
            InterlockedExchange(&init_done, 1);
        }
    }
    return c;
}

static int si_filter(sqlite3_vtab_cursor* cur, int idxNum, const char* idxStr,
                     int argc, sqlite3_value** argv)
{
    VmsSiCursor* c = (VmsSiCursor*)cur;
    VmsSiVtab* v = (VmsSiVtab*)c->base.pVtab;
    const char* order = idxStr;
    wchar_t conn_spec[1024] = L"";
    wchar_t schema[300] = L"";
    wchar_t table[300] = L"";
    wchar_t index[300] = L"";
    VmsProfile profile;
    VmsError err;
    wchar_t* connstr = NULL;
    size_t connstr_len = 0;
    VmsConnection* cn = NULL;
    VmsCursor* rows = NULL;
    char spec_u8[2048];
    wchar_t sql[4096];
    int rc = SQLITE_ERROR;

    c->cur = NULL;
    c->eof = 1;
    memset(&err, 0, sizeof(err));

    if (idxNum == 0 || !order || argc < 1) {
        c->base.pVtab->zErrMsg = sqlite3_mprintf(
            "virtualmssql: connection specification is required");
        return SQLITE_ERROR;
    }
    /* connection spec (hidden col 0) — required */
    if (order[0] > 0 && order[0] <= argc) {
        if (!si_arg_w(argv[order[0] - 1], conn_spec, 1024)) {
            c->base.pVtab->zErrMsg = sqlite3_mprintf(
                "virtualmssql: connection specification must be non-empty text");
            return SQLITE_ERROR;
        }
    }
    if (!conn_spec[0]) {
        c->base.pVtab->zErrMsg = sqlite3_mprintf(
            "virtualmssql: connection specification is required");
        return SQLITE_ERROR;
    }
    /* schema (hidden col 1) — required for all kinds */
    if (order[1] > 0 && order[1] <= argc) {
        if (!si_arg_w(argv[order[1] - 1], schema, 300) || !schema[0]) {
            c->base.pVtab->zErrMsg = sqlite3_mprintf(
                "virtualmssql: schema must be a non-empty text value");
            return SQLITE_ERROR;
        }
    }
    /* table (hidden col 2) — required for kinds 1, 2, 3 */
    if (v->kind != SI_TABLES) {
        if (order[2] > 0 && order[2] <= argc) {
            if (!si_arg_w(argv[order[2] - 1], table, 300) || !table[0]) {
                c->base.pVtab->zErrMsg = sqlite3_mprintf(
                    "virtualmssql: table must be a non-empty text value");
                return SQLITE_ERROR;
            }
        }
    }
    /* index (hidden col 3) — required for kind 3 */
    if (v->kind == SI_INDEX_INFO) {
        if (order[3] > 0 && order[3] <= argc) {
            if (!si_arg_w(argv[order[3] - 1], index, 300) || !index[0]) {
                c->base.pVtab->zErrMsg = sqlite3_mprintf(
                    "virtualmssql: index must be a non-empty text value");
                return SQLITE_ERROR;
            }
        }
    }
    /* identifiers must be catalog-safe before quoting */
    if (!vms_meta_ident_valid(schema, 128) ||
        (v->kind != SI_TABLES && !vms_meta_ident_valid(table, 128)) ||
        (v->kind == SI_INDEX_INFO && !vms_meta_ident_valid(index, 128))) {
        c->base.pVtab->zErrMsg = sqlite3_mprintf(
            "virtualmssql: invalid identifier in schema inspection arguments");
        return SQLITE_ERROR;
    }
    if (!si_build_sql(v->kind, schema, table, index, sql, 4096)) {
        c->base.pVtab->zErrMsg = sqlite3_mprintf(
            "virtualmssql: schema inspection query build failed");
        return SQLITE_ERROR;
    }
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, conn_spec, -1, spec_u8,
                                    (int)sizeof(spec_u8), NULL, NULL);
        VmsError perr;
        memset(&perr, 0, sizeof(perr));
        if (n <= 0 || !vms_profile_parse(spec_u8, &profile, &perr)) {
            c->base.pVtab->zErrMsg = sqlite3_mprintf(
                "virtualmssql: bad connection specification: %s",
                perr.message);
            return SQLITE_ERROR;
        }
    }
    if (!vms_connstr_build(&profile, &connstr, &connstr_len, &err)) {
        si_set_error(c->base.pVtab, &err);
        return SQLITE_ERROR;
    }
    cn = vms_conn_open(si_client(), connstr, &err);
    vms_connstr_free(connstr);
    if (!cn) {
        si_set_error(c->base.pVtab, &err);
        return SQLITE_ERROR;
    }
    rows = vms_cursor_open_sql(cn, sql, NULL, 0, &err);
    vms_conn_close(cn); /* the cursor owns its independent HDBC */
    if (!rows) {
        si_set_error(c->base.pVtab, &err);
        return SQLITE_ERROR;
    }
    c->cur = rows;
    c->eof = 0;
    {
        VmsError ferr;
        int r;
        memset(&ferr, 0, sizeof(ferr));
        r = vms_cursor_fetch(c->cur, &ferr);
        if (r < 0) {
            c->base.pVtab->zErrMsg = sqlite3_mprintf("virtualmssql: %s",
                                                     ferr.message);
            vms_cursor_close(c->cur);
            c->cur = NULL;
            c->eof = 1;
            return SQLITE_ERROR;
        }
        c->eof = (r == 0);
    }
    (void)rc;
    return SQLITE_OK;
}

static int si_step(sqlite3_vtab_cursor* cur)
{
    VmsSiCursor* c = (VmsSiCursor*)cur;
    VmsError err;
    int r;
    memset(&err, 0, sizeof(err));
    r = vms_cursor_fetch(c->cur, &err);
    if (r < 0) {
        c->base.pVtab->zErrMsg = sqlite3_mprintf("virtualmssql: %s",
                                                 err.message);
        c->eof = 1;
        return SQLITE_ERROR;
    }
    c->eof = (r == 0);
    return SQLITE_OK;
}

static int si_eof(sqlite3_vtab_cursor* cur)
{
    return ((VmsSiCursor*)cur)->eof;
}

static int si_column(sqlite3_vtab_cursor* cur, sqlite3_context* ctx, int col)
{
    VmsSiCursor* c = (VmsSiCursor*)cur;
    VmsSiVtab* v = (VmsSiVtab*)c->base.pVtab;
    int remote = col - k_si_hidden[v->kind];
    if (remote < 0) { sqlite3_result_null(ctx); return SQLITE_OK; }
    {
        const VmsValue* val = vms_cursor_value(c->cur, remote);
        if (!val || val->type == VMS_VAL_NULL) {
            sqlite3_result_null(ctx);
            return SQLITE_OK;
        }
        switch (val->type) {
        case VMS_VAL_INT64:
            sqlite3_result_int64(ctx, (sqlite3_int64)val->i);
            break;
        case VMS_VAL_FLOAT64:
            sqlite3_result_double(ctx, val->f);
            break;
        case VMS_VAL_TEXT:
            sqlite3_result_text(ctx, val->text, (int)val->text_len,
                                SQLITE_TRANSIENT);
            break;
        case VMS_VAL_BLOB:
            sqlite3_result_blob(ctx, val->blob, (int)val->blob_len,
                                SQLITE_TRANSIENT);
            break;
        default:
            sqlite3_result_null(ctx);
            break;
        }
    }
    return SQLITE_OK;
}

static int si_rowid(sqlite3_vtab_cursor* cur, sqlite3_int64* rowid)
{
    static sqlite3_int64 counter = 0;
    *rowid = ++counter;
    (void)cur;
    return SQLITE_OK;
}

static int si_close(sqlite3_vtab_cursor* cur)
{
    VmsSiCursor* c = (VmsSiCursor*)cur;
    if (c->cur) vms_cursor_close(c->cur);
    sqlite3_free(cur);
    return SQLITE_OK;
}

static int si_update(sqlite3_vtab* vtab, int argc, sqlite3_value** argv,
                     sqlite3_int64* rowid)
{
    (void)argc; (void)argv; (void)rowid;
    vtab->zErrMsg = sqlite3_mprintf(
        "virtualmssql: schema inspection functions are read-only");
    return SQLITE_READONLY;
}

static int si_begin(sqlite3_vtab* vtab) { (void)vtab; return SQLITE_OK; }
static int si_sync(sqlite3_vtab* vtab) { (void)vtab; return SQLITE_OK; }
static int si_commit(sqlite3_vtab* vtab) { (void)vtab; return SQLITE_OK; }
static int si_rollback(sqlite3_vtab* vtab) { (void)vtab; return SQLITE_OK; }

static int si_open(sqlite3_vtab* vtab, sqlite3_vtab_cursor** ppCursor)
{
    VmsSiCursor* c = (VmsSiCursor*)sqlite3_malloc(sizeof(VmsSiCursor));
    if (!c) return SQLITE_NOMEM;
    memset(c, 0, sizeof(*c));
    c->base.pVtab = vtab;
    *ppCursor = &c->base;
    return SQLITE_OK;
}

static sqlite3_module si_tables_module = {
    0, si_connect, si_connect, si_best_index, si_disconnect, si_disconnect,
    si_open, si_close, si_filter, si_step, si_eof, si_column, si_rowid,
    si_update, si_begin, si_sync, si_commit, si_rollback, 0, 0, 0, 0, 0, 0, 0
};
static sqlite3_module si_table_info_module = {
    0, si_connect, si_connect, si_best_index, si_disconnect, si_disconnect,
    si_open, si_close, si_filter, si_step, si_eof, si_column, si_rowid,
    si_update, si_begin, si_sync, si_commit, si_rollback, 0, 0, 0, 0, 0, 0, 0
};
static sqlite3_module si_index_list_module = {
    0, si_connect, si_connect, si_best_index, si_disconnect, si_disconnect,
    si_open, si_close, si_filter, si_step, si_eof, si_column, si_rowid,
    si_update, si_begin, si_sync, si_commit, si_rollback, 0, 0, 0, 0, 0, 0, 0
};
static sqlite3_module si_index_info_module = {
    0, si_connect, si_connect, si_best_index, si_disconnect, si_disconnect,
    si_open, si_close, si_filter, si_step, si_eof, si_column, si_rowid,
    si_update, si_begin, si_sync, si_commit, si_rollback, 0, 0, 0, 0, 0, 0, 0
};

int vms_schema_register_all(sqlite3* db)
{
    static sqlite3_module* k_modules[] = {
        &si_tables_module, &si_table_info_module,
        &si_index_list_module, &si_index_info_module
    };
    static const char* k_names[] = {
        "virtualmssql_tables", "virtualmssql_table_info",
        "virtualmssql_index_list", "virtualmssql_index_info"
    };
    int i;
    for (i = 0; i < 4; i++) {
        if (sqlite3_create_module(db, k_names[i], k_modules[i],
                                  (void*)(intptr_t)i) != SQLITE_OK)
            return SQLITE_ERROR;
    }
    return SQLITE_OK;
}
