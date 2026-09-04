/* vms_meta.c — metadata / type registry / stable identity (R5).
 * Catalog reads go through the R3 client layer (worker-serialized);
 * identifiers are validated and N''-escaped before entering any query. */
#include "vms_meta.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ---- query helper ---- */

static const VmsValue* colv(VmsStatement* st, int i)
{
    return vms_stmt_value(st, i);
}

static void copy_text(char* dst, size_t cap, const VmsValue* v)
{
    if (v && v->type == VMS_VAL_TEXT) {
        strncpy_s(dst, cap, v->text, _TRUNCATE);
    } else {
        dst[0] = 0;
    }
}

static long long get_i64(const VmsValue* v, long long dflt)
{
    return (v && v->type == VMS_VAL_INT64) ? v->i : dflt;
}

typedef int (*RowFn)(void* user, VmsStatement* st);

static int run_query(VmsConnection* cn, const wchar_t* sql, RowFn row, void* user,
                     VmsError* err)
{
    VmsStatement* st = vms_stmt_exec_direct(cn, sql, err);
    int rc = 1;
    if (!st) return 0;
    for (;;) {
        int r = vms_stmt_fetch(st, err);
        if (r < 0) { rc = 0; break; }
        if (r == 0) break;
        if (!row(user, st)) { rc = 0; break; }
    }
    vms_stmt_destroy(st);
    return rc;
}

/* ---- type registry: sys.types name -> VmsColType ---- */

static VmsColType vtype_from_sql_type(const char* t, unsigned long max_length)
{
    if (!strcmp(t, "bigint") || !strcmp(t, "int") || !strcmp(t, "smallint") ||
        !strcmp(t, "tinyint") || !strcmp(t, "bit")) return VMS_CT_INT64;
    if (!strcmp(t, "float") || !strcmp(t, "real")) return VMS_CT_FLOAT64;
    if (!strcmp(t, "decimal") || !strcmp(t, "numeric") ||
        !strcmp(t, "money") || !strcmp(t, "smallmoney")) return VMS_CT_DECIMAL;
    if (!strcmp(t, "date") || !strcmp(t, "time") || !strcmp(t, "datetime") ||
        !strcmp(t, "datetime2") || !strcmp(t, "smalldatetime") ||
        !strcmp(t, "datetimeoffset")) return VMS_CT_DATETIME;
    if (!strcmp(t, "uniqueidentifier")) return VMS_CT_GUID;
    if (!strcmp(t, "geometry") || !strcmp(t, "geography")) return VMS_CT_SPATIAL;
    if (!strcmp(t, "binary") || !strcmp(t, "varbinary") ||
        !strcmp(t, "image") || !strcmp(t, "rowversion") ||
        !strcmp(t, "timestamp")) return VMS_CT_BLOB;
    if (!strcmp(t, "nvarchar") || !strcmp(t, "nchar") || !strcmp(t, "ntext") ||
        !strcmp(t, "varchar") || !strcmp(t, "char") || !strcmp(t, "text") ||
        !strcmp(t, "xml")) {
        if (max_length == (unsigned long)-1 || max_length == 0) return VMS_CT_BIGTEXT;
        return VMS_CT_TEXT;
    }
    /* R12: deterministic UNSUPPORTED_TYPE policy — types without a lossless
     * mapping (sql_variant, hierarchyid, ...) fail the table instead of
     * silently degrading to TEXT */
    return VMS_CT_UNSUPPORTED;
}

/* ---- object kind ---- */

typedef struct KindCtx {
    VmsObjKind kind;
} KindCtx;

static int kind_row(void* user, VmsStatement* st)
{
    KindCtx* c = (KindCtx*)user;
    long long k = get_i64(colv(st, 0), 0);
    c->kind = (k == 1) ? VMS_OBJ_TABLE : (k == 2) ? VMS_OBJ_VIEW : VMS_OBJ_ABSENT;
    return 1;
}

VmsObjKind vms_meta_object_kind(VmsConnection* cn, const char* schema,
                                const char* name, VmsError* err)
{
    wchar_t sql[512];
    char s_lit[300], n_lit[300];
    KindCtx ctx;

    if (!vms_meta_ident_valid(schema, VMS_META_MAX_NAME) ||
        !vms_meta_ident_valid(name, VMS_META_MAX_NAME)) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "invalid identifier");
        return VMS_OBJ_ABSENT;
    }
    vms_meta_quote_ident(schema, s_lit, sizeof(s_lit));
    vms_meta_quote_ident(name, n_lit, sizeof(n_lit));
    _snwprintf_s(sql, 512, _TRUNCATE,
        L"SELECT CASE o.type WHEN N'U' THEN 1 WHEN N'V' THEN 2 ELSE 0 END "
        L"FROM sys.objects o JOIN sys.schemas s ON s.schema_id = o.schema_id "
        L"WHERE s.name = %hs AND o.name = %hs", s_lit, n_lit);

    ctx.kind = VMS_OBJ_ABSENT;
    if (!run_query(cn, sql, kind_row, &ctx, err)) {
        return VMS_OBJ_ABSENT;
    }
    return ctx.kind;
}

/* ---- columns ---- */

typedef struct ColsCtx {
    VmsTableColumns* out;
    int full;
} ColsCtx;

static int cols_row(void* user, VmsStatement* st)
{
    ColsCtx* c = (ColsCtx*)user;
    VmsMetaColumn* m;
    char type_name[64];
    const VmsValue* v;

    if (c->out->count >= VMS_META_MAX_COLUMNS) return 0; /* stop: too wide */
    m = &c->out->cols[c->out->count];
    memset(m, 0, sizeof(*m));
    copy_text(m->name, sizeof(m->name), colv(st, 0));
    copy_text(type_name, sizeof(type_name), colv(st, 1));
    strncpy_s(m->type_name, sizeof(m->type_name), type_name, _TRUNCATE);
    m->max_length = (unsigned long)get_i64(colv(st, 2), 0);
    m->precision = (unsigned char)get_i64(colv(st, 3), 0);
    m->scale = (unsigned char)get_i64(colv(st, 4), 0);
    m->is_nullable = (int)get_i64(colv(st, 5), 0);
    m->is_identity = (int)get_i64(colv(st, 6), 0);
    m->is_computed = (int)get_i64(colv(st, 7), 0);
    m->vtype = vtype_from_sql_type(type_name, m->max_length);
    c->out->count++;
    v = NULL;
    return 1;
}

int vms_meta_columns(VmsConnection* cn, const char* schema, const char* name,
                     VmsTableColumns* out, VmsError* err)
{
    wchar_t* sql = NULL;
    char s_lit[300], n_lit[300];
    ColsCtx ctx;
    int ok;

    if (!vms_meta_ident_valid(schema, VMS_META_MAX_NAME) ||
        !vms_meta_ident_valid(name, VMS_META_MAX_NAME)) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "invalid identifier");
        return 0;
    }
    vms_meta_quote_ident(schema, s_lit, sizeof(s_lit));
    vms_meta_quote_ident(name, n_lit, sizeof(n_lit));
    {
        wchar_t buf[1024];
        _snwprintf_s(buf, 1024, _TRUNCATE,
            L"SELECT c.name, t.name, c.max_length, c.precision, c.scale, "
            L"c.is_nullable, c.is_identity, c.is_computed "
            L"FROM sys.columns c "
            L"JOIN sys.types t ON t.user_type_id = c.user_type_id "
            L"JOIN sys.objects o ON o.object_id = c.object_id "
            L"JOIN sys.schemas s ON s.schema_id = o.schema_id "
            L"WHERE s.name = %hs AND o.name = %hs ORDER BY c.column_id",
            s_lit, n_lit);
        sql = _wcsdup(buf);
        if (!sql) {
            vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM meta query");
            return 0;
        }
    }
    memset(out, 0, sizeof(*out));
    ctx.out = out;
    ctx.full = 0;
    ok = run_query(cn, sql, cols_row, &ctx, err);
    free(sql);
    return ok;
}

/* ---- stable key ---- */

typedef struct KeyCtx {
    /* candidates accumulate per index; PK wins */
    VmsStableKey best;
    VmsStableKey current;
    long long current_index_id;
    int current_suitable;
    int have_best;
} KeyCtx;

static void key_finish_current(KeyCtx* k)
{
    if (k->current_index_id == 0) return;
    if (k->current_suitable && k->current.part_count > 0) {
        int better = 0;
        if (!k->have_best) better = 1;
        else if (k->current.is_primary_key && !k->best.is_primary_key) better = 1;
        if (better) {
            k->best = k->current;
            k->have_best = 1;
        }
    }
    k->current_index_id = 0;
    k->current_suitable = 0;
    memset(&k->current, 0, sizeof(k->current));
}

static int key_row(void* user, VmsStatement* st)
{
    KeyCtx* k = (KeyCtx*)user;
    long long index_id = get_i64(colv(st, 0), 0);
    long long is_pk = get_i64(colv(st, 1), 0);
    long long suitable = get_i64(colv(st, 2), 0);
    long long ordinal = get_i64(colv(st, 3), 0);
    char col_name[VMS_META_MAX_NAME];
    char type_name[64];
    VmsMetaColumn tmpcol;

    copy_text(col_name, sizeof(col_name), colv(st, 4));
    copy_text(type_name, sizeof(type_name), colv(st, 5));
    tmpcol.max_length = (unsigned long)get_i64(colv(st, 6), 0);
    tmpcol.vtype = vtype_from_sql_type(type_name, tmpcol.max_length);

    if (index_id != k->current_index_id) {
        key_finish_current(k);
        k->current_index_id = index_id;
        k->current_suitable = (suitable == 1);
        k->current.is_primary_key = (is_pk == 1);
        copy_text(k->current.index_name, sizeof(k->current.index_name),
                  colv(st, 7));
    }
    if (!k->current_suitable) return 1;
    if (ordinal < 1 || ordinal > VMS_META_MAX_KEY_PARTS) {
        k->current_suitable = 0;
        return 1;
    }
    strncpy_s(k->current.parts[ordinal - 1].name,
              sizeof(k->current.parts[ordinal - 1].name), col_name, _TRUNCATE);
    k->current.parts[ordinal - 1].vtype = tmpcol.vtype;
    if (ordinal > k->current.part_count) k->current.part_count = (int)ordinal;
    return 1;
}

int vms_meta_stable_key(VmsConnection* cn, const char* schema,
                        const char* name, VmsStableKey* out, VmsError* err)
{
    wchar_t buf[1280];
    char s_lit[300], n_lit[300];
    KeyCtx k;
    int ok;

    if (!vms_meta_ident_valid(schema, VMS_META_MAX_NAME) ||
        !vms_meta_ident_valid(name, VMS_META_MAX_NAME)) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "invalid identifier");
        return 0;
    }
    vms_meta_quote_ident(schema, s_lit, sizeof(s_lit));
    vms_meta_quote_ident(name, n_lit, sizeof(n_lit));
    /* suitability columns: i.is_unique, i.is_disabled, i.is_hypothetical,
     * i.has_filter, plus column checks: nullable/computed key columns */
    _snwprintf_s(buf, 1280, _TRUNCATE,
        L"SELECT i.index_id, i.is_primary_key, "
        L"CASE WHEN i.is_disabled = 0 AND i.is_hypothetical = 0 "
        L"      AND i.has_filter = 0 "
        L"      AND NOT EXISTS (SELECT 1 FROM sys.index_columns ic2 "
        L"                      JOIN sys.columns c2 ON c2.object_id = ic2.object_id "
        L"                                        AND c2.column_id = ic2.column_id "
        L"                      WHERE ic2.object_id = i.object_id "
        L"                        AND ic2.index_id = i.index_id "
        L"                        AND ic2.key_ordinal > 0 "
        L"                        AND (c2.is_nullable = 1 OR c2.is_computed = 1)) "
        L"     THEN 1 ELSE 0 END, "
        L"ic.key_ordinal, c.name, t.name, c.max_length, i.name "
        L"FROM sys.indexes i "
        L"JOIN sys.index_columns ic ON ic.object_id = i.object_id AND ic.index_id = i.index_id "
        L"JOIN sys.columns c ON c.object_id = ic.object_id AND c.column_id = ic.column_id "
        L"JOIN sys.types t ON t.user_type_id = c.user_type_id "
        L"JOIN sys.objects o ON o.object_id = i.object_id "
        L"JOIN sys.schemas s ON s.schema_id = o.schema_id "
        L"WHERE s.name = %hs AND o.name = %hs AND i.is_unique = 1 "
        L"ORDER BY i.index_id, ic.key_ordinal",
        s_lit, n_lit);

    memset(&k, 0, sizeof(k));
    ok = run_query(cn, buf, key_row, &k, err);
    key_finish_current(&k);
    if (!ok) return 0;
    if (!k.have_best) return 0;
    *out = k.best;
    return 1;
}

static int trig_row(void* user, VmsStatement* st)
{
    VmsTriggerList* c = (VmsTriggerList*)user;
    if (c->count >= 16) return 0;
    copy_text(c->names[c->count], VMS_META_MAX_NAME, colv(st, 0));
    copy_text(c->events[c->count], 32, colv(st, 1));
    c->count++;
    return 1;
}

int vms_meta_triggers(VmsConnection* cn, const char* schema,
                      const char* table, VmsTriggerList* out, VmsError* err)
{
    wchar_t buf[1024];
    char s_lit[300], t_lit[300];
    int ok;

    if (!vms_meta_ident_valid(schema, VMS_META_MAX_NAME) ||
        !vms_meta_ident_valid(table, VMS_META_MAX_NAME)) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "invalid identifier");
        return 0;
    }
    vms_meta_quote_ident(schema, s_lit, sizeof(s_lit));
    vms_meta_quote_ident(table, t_lit, sizeof(t_lit));
    _snwprintf_s(buf, 1024, _TRUNCATE,
        L"SELECT tr.name, tr.is_disabled "
        L"FROM sys.triggers tr "
        L"JOIN sys.objects o ON o.object_id = tr.parent_id "
        L"JOIN sys.schemas s ON s.schema_id = o.schema_id "
        L"WHERE s.name = %hs AND o.name = %hs", s_lit, t_lit);
    memset(out, 0, sizeof(*out));
    ok = run_query(cn, buf, trig_row, out, err);
    return ok;
}
