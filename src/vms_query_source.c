/* vms_query_source.c — source=query vtab support (R8).
 * Validation: bounded lexer; Description: sys.sp_describe_first_result_set;
 * Contract: unique column names, one row-producing result set (outer
 * wrapper). */
#include "vms_query_source.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* describe via sp_describe_first_result_set; read through the client layer */
typedef struct DescCtx {
    VmsQuerySource* src;
    int ok;
} DescCtx;

static int desc_row(void* user, VmsStatement* st)
{
    DescCtx* d = (DescCtx*)user;
    VmsQuerySource* src = d->src;
    VmsMetaColumn* m;
    const VmsValue* v;
    char type_name[64];
    int j;

    if (src->ncols >= 512) return 0; /* too wide */
    m = &src->cols[src->ncols];
    memset(m, 0, sizeof(*m));

    /* sys.dm_exec_describe_first_result_set columns:
     * 0 name, 1 system_type_name, 2 max_length, 3 precision, 4 scale,
     * 5 is_nullable */
    v = vms_stmt_value(st, 0);
    if (v && v->type == VMS_VAL_TEXT) {
        strncpy_s(m->name, sizeof(m->name), v->text, _TRUNCATE);
    }
    type_name[0] = 0;
    v = vms_stmt_value(st, 1);
    if (v && v->type == VMS_VAL_TEXT) {
        strncpy_s(type_name, sizeof(type_name), v->text, _TRUNCATE);
    }
    v = vms_stmt_value(st, 2);
    m->max_length = (v && v->type == VMS_VAL_INT64) ? (unsigned long)v->i : 0;
    v = vms_stmt_value(st, 3);
    m->precision = (v && v->type == VMS_VAL_INT64) ? (unsigned char)v->i : 0;
    v = vms_stmt_value(st, 4);
    m->scale = (v && v->type == VMS_VAL_INT64) ? (unsigned char)v->i : 0;
    v = vms_stmt_value(st, 5);
    m->is_nullable = (v && v->type == VMS_VAL_INT64) ? (int)v->i : 1;

    /* registry mapping (same as table metadata) */
    if (!strcmp(type_name, "bigint") || !strcmp(type_name, "int") ||
        !strcmp(type_name, "smallint") || !strcmp(type_name, "tinyint") ||
        !strcmp(type_name, "bit")) m->vtype = VMS_CT_INT64;
    else if (!strcmp(type_name, "float") || !strcmp(type_name, "real")) m->vtype = VMS_CT_FLOAT64;
    else if (!strcmp(type_name, "decimal") || !strcmp(type_name, "numeric") ||
             !strcmp(type_name, "money") || !strcmp(type_name, "smallmoney")) m->vtype = VMS_CT_DECIMAL;
    else if (!strcmp(type_name, "date") || !strcmp(type_name, "time") ||
             !strcmp(type_name, "datetime") || !strcmp(type_name, "datetime2") ||
             !strcmp(type_name, "smalldatetime") || !strcmp(type_name, "datetimeoffset")) m->vtype = VMS_CT_DATETIME;
    else if (!strcmp(type_name, "uniqueidentifier")) m->vtype = VMS_CT_GUID;
    else if (!strcmp(type_name, "binary") || !strcmp(type_name, "varbinary") ||
             !strcmp(type_name, "image") || !strcmp(type_name, "rowversion")) m->vtype = VMS_CT_BLOB;
    else if (!strcmp(type_name, "xml")) m->vtype = VMS_CT_BIGTEXT;
    else if (!strcmp(type_name, "nvarchar") || !strcmp(type_name, "nchar") ||
             !strcmp(type_name, "varchar") || !strcmp(type_name, "char") ||
             !strcmp(type_name, "text") || !strcmp(type_name, "ntext")) {
        m->vtype = (m->max_length == (unsigned long)-1 || m->max_length == 0)
                       ? VMS_CT_BIGTEXT : VMS_CT_TEXT;
    }
    else m->vtype = VMS_CT_TEXT;

    /* unique column names contract */
    for (j = 0; j < src->ncols; j++) {
        if (!_stricmp(src->cols[j].name, m->name)) {
            return 0; /* duplicate */
        }
    }

    src->ncols++;
    fflush(stderr);
    return 1;
}

int vms_query_source_prepare(VmsConnection* cn, const char* query_utf8,
                             VmsQuerySource* src, VmsError* err)
{
    wchar_t query_w[32768];
    wchar_t sql[34816];
    char errbuf[256];
    DescCtx dctx;
    int ok;
    VmsStatement* st;

    memset(src, 0, sizeof(*src));
    vms_error_ok(err);
    if (!cn || !query_utf8) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "query source: bad args");
        return 0;
    }
    {
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    query_utf8, -1, query_w, 32768);
        if (n <= 0) {
            vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0,
                          "query is not valid UTF-8");
            return 0;
        }
    }

    /* stage 1: bounded lexer validation */
    if (!vms_tsql_validate_query(query_w, errbuf, sizeof(errbuf))) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0,
                      "query rejected: %s", errbuf);
        return 0;
    }

    /* stage 2: describe the first result set (server-side contract) */
    _snwprintf_s(sql, 34816, _TRUNCATE,
                 L"EXEC sys.sp_describe_first_result_set @tsql = N'%ls'",
                 query_w);
    /* the query itself cannot contain a lone quote-terminator: the lexer
     * verified balanced strings; embedded quotes are doubled when the
     * server parses the literal — we must double them here */
    {
        /* simple literal doubling pass over query_w inside N'...' */
        wchar_t wrapped[34816];
        size_t w = 0, q = 0;
        wrapped[w++] = L'N'; wrapped[w++] = L'\'';
        for (q = 0; query_w[q] && w < 34800; q++) {
            wrapped[w++] = query_w[q];
            if (query_w[q] == L'\'') wrapped[w++] = L'\'';
        }
        wrapped[w++] = L'\'';
        wrapped[w] = 0;
        _snwprintf_s(sql, 34816, _TRUNCATE,
                     L"SELECT name, system_type_name, max_length, precision,"
                     L" scale, is_nullable FROM"
                     L" sys.dm_exec_describe_first_result_set(%ls, NULL, 0)",
                     wrapped);
    }

    st = vms_stmt_exec_direct(cn, sql, err);
    if (!st) return 0;
    memset(&dctx, 0, sizeof(dctx));
    dctx.src = src;
    ok = 1;
    for (;;) {
        int r = vms_stmt_fetch(st, err);
        if (r < 0) { ok = 0; break; }
        if (r == 0) break;
        if (!desc_row(&dctx, st)) { ok = 0; break; }
    }
    vms_stmt_destroy(st);
    if (!ok) {
        if (err->cls == VMS_OK)
            vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0,
                          "query result contract violated (duplicate or too many columns)");
        return 0;
    }
    if (src->ncols < 1) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0,
                      "query produces no result set");
        return 0;
    }

    /* keep the validated query */
    {
        size_t n = wcslen(query_w) + 1;
        src->query = (wchar_t*)malloc(n * sizeof(wchar_t));
        if (!src->query) {
            vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM query source");
            return 0;
        }
        memcpy(src->query, query_w, n * sizeof(wchar_t));
    }
    fflush(stderr);
    return 1;
}

int vms_query_source_get_sql(const VmsQuerySource* src,
                             wchar_t* out, size_t out_wchars)
{
    size_t n;
    if (!src || !src->query || !out) return 0;
    n = wcslen(src->query);
    if (n + 1 > out_wchars) return 0;
    memcpy(out, src->query, (n + 1) * sizeof(wchar_t));
    return 1;
}

void vms_query_source_free(VmsQuerySource* src)
{
    if (!src) return;
    if (src->query) { free(src->query); src->query = NULL; }
    src->ncols = 0;
}
