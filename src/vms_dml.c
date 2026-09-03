/* vms_dml.c — write path for source=table mode=rw virtual tables (R10).
 *
 * Statement generation: bracket-quoted validated identifiers; int64 values
 * are inlined as canonical digits (no injection surface), text values as
 * N'...' with doubled quotes after conversion. The stable key is validated
 * at vms_dml_init; identity/computed/rowversion columns are never written.
 */
#include "vms_dml.h"
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define VMS_DML_SQL_CAP 4096
#define VMS_DML_MAX_PARAMS 64

static int col_writable(const VmsMetaColumn* c)
{
    return !c->is_identity && !c->is_computed &&
           _stricmp(c->type_name, "rowversion") != 0 &&
           _stricmp(c->type_name, "timestamp") != 0;
}

static int col_is_rowversion(const VmsMetaColumn* c)
{
    return _stricmp(c->type_name, "rowversion") == 0 ||
           _stricmp(c->type_name, "timestamp") == 0;
}

int vms_dml_init(VmsDmlContext* d, VmsConnection* cn, const char* schema,
                 const char* table, VmsMetaColumn* cols, int ncols,
                 VmsError* err)
{
    int i;
    memset(d, 0, sizeof(*d));
    vms_error_ok(err);
    if (!cn || !vms_meta_ident_valid(schema, 128) ||
        !vms_meta_ident_valid(table, 128)) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "dml init: bad args");
        return 0;
    }
    d->cn = cn;
    strncpy_s(d->schema, sizeof(d->schema), schema, _TRUNCATE);
    strncpy_s(d->table, sizeof(d->table), table, _TRUNCATE);
    d->cols = cols;
    d->ncols = ncols;
    d->rowversion_col = -1;
    for (i = 0; i < ncols; i++) {
        if (col_is_rowversion(&cols[i]) && d->rowversion_col < 0)
            d->rowversion_col = i;
    }
    if (!vms_meta_stable_key(cn, schema, table, &d->key, err)) {
        if (err->cls == VMS_OK)
            vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0,
                          "table has no usable stable key: writes are not allowed");
        return 0;
    }
    return 1;
}

static int key_part_col(VmsDmlContext* d, int part)
{
    int j;
    for (j = 0; j < d->ncols; j++) {
        if (!_stricmp(d->cols[j].name, d->key.parts[part].name)) return j;
    }
    return -1;
}

static int is_key_col(VmsDmlContext* d, int col)
{
    int k;
    for (k = 0; k < d->key.part_count; k++) {
        if (key_part_col(d, k) == col) return 1;
    }
    return 0;
}

typedef struct SqlB {
    wchar_t* s;
    size_t cap;
    size_t len;
} SqlB;

static int sb_add(SqlB* b, const wchar_t* s)
{
    size_t wl = wcslen(s);
    if (b->len + wl + 1 > b->cap) return 0;
    memcpy(b->s + b->len, s, (wl + 1) * sizeof(wchar_t));
    b->len += wl;
    return 1;
}

static int sb_add_ident(SqlB* b, const char* name)
{
    wchar_t w[280];
    int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, w + 1, 276);
    size_t wl;
    if (n <= 0) return 0;
    w[0] = L'[';
    w[n] = L']';
    w[n + 1] = 0;
    wl = wcslen(w);
    if (b->len + wl + 1 > b->cap) return 0;
    memcpy(b->s + b->len, w, (wl + 1) * sizeof(wchar_t));
    b->len += wl;
    return 1;
}

static int sb_add_table(SqlB* b, const char* schema, const char* table)
{
    wchar_t w[300];
    int n;
    if (!sb_add(b, L"[")) return 0;
    n = MultiByteToWideChar(CP_UTF8, 0, schema, -1, w, 300);
    if (n <= 0) return 0;
    if (!sb_add(b, w)) return 0;
    if (!sb_add(b, L"].[")) return 0;
    n = MultiByteToWideChar(CP_UTF8, 0, table, -1, w, 300);
    if (n <= 0) return 0;
    if (!sb_add(b, w)) return 0;
    return sb_add(b, L"]");
}

/* append an SQL literal for a VmsValue */
static int sb_add_value(SqlB* b, const VmsValue* tmp)
{
    if (tmp->type == VMS_VAL_INT64) {
        wchar_t num[24];
        _snwprintf_s(num, 24, _TRUNCATE, L"%lld", tmp->i);
        return sb_add(b, num);
    }
    if (tmp->type == VMS_VAL_FLOAT64) {
        wchar_t num[40];
        _snwprintf_s(num, 40, _TRUNCATE, L"%.17g", tmp->f);
        return sb_add(b, num);
    }
    if (tmp->type == VMS_VAL_TEXT) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, tmp->text,
                                       (int)tmp->text_len, NULL, 0);
        wchar_t* wv = (wchar_t*)malloc((size_t)(wlen + 1) * sizeof(wchar_t));
        int k;
        if (!wv) return 0;
        MultiByteToWideChar(CP_UTF8, 0, tmp->text, (int)tmp->text_len, wv, wlen);
        wv[wlen] = 0;
        if (!sb_add(b, L"N'")) { free(wv); return 0; }
        for (k = 0; k < wlen; k++) {
            if (wv[k] == L'\'') { if (!sb_add(b, L"''")) { free(wv); return 0; } }
            else {
                wchar_t one[2] = { wv[k], 0 };
                if (!sb_add(b, one)) { free(wv); return 0; }
            }
        }
        if (!sb_add(b, L"'")) { free(wv); return 0; }
        free(wv);
        return 1;
    }
    return sb_add(b, L"NULL");
}

static int text_to_value(const char* text, int is_null, VmsColType vtype,
                         VmsValue* out)
{
    memset(out, 0, sizeof(*out));
    if (is_null || !text) { out->type = VMS_VAL_NULL; return 1; }
    switch (vtype) {
    case VMS_CT_INT64: {
        char* end;
        out->type = VMS_VAL_INT64;
        out->i = _strtoi64(text, &end, 10);
        if (end == text) return 0;
        return 1;
    }
    case VMS_CT_FLOAT64: {
        char* end;
        out->type = VMS_VAL_FLOAT64;
        out->f = strtod(text, &end);
        if (end == text) return 0;
        return 1;
    }
    default:
        out->type = VMS_VAL_TEXT;
        out->text_len = strlen(text);
        out->text = _strdup(text);
        return out->text != NULL;
    }
}

int vms_dml_insert(VmsDmlContext* d, const unsigned char* col_present,
                   VmsDmlValueGet get, void* user,
                   long long* rows_affected, VmsError* err)
{
    wchar_t sql[VMS_DML_SQL_CAP];
    SqlB b;
    int i, first = 1, np = 0;

    vms_error_ok(err);
    sql[0] = 0; b.s = sql; b.cap = VMS_DML_SQL_CAP; b.len = 0;
    if (!sb_add(&b, L"INSERT INTO ") || !sb_add_table(&b, d->schema, d->table) ||
        !sb_add(&b, L" (")) return -1;
    for (i = 0; i < d->ncols; i++) {
        int is_null = 0;
        const char* text;
        if (!col_writable(&d->cols[i])) continue;
        /* NULL key columns are omitted so server-side defaults apply
         * (e.g. uniqueidentifier PK with DEFAULT NEWID()) */
        if (is_key_col(d, i)) {
            text = get(user, i, &is_null);
            if (is_null || !text || !text[0]) continue;
        }
        if (col_present && !col_present[i]) continue;
        if (!first && !sb_add(&b, L", ")) return -1;
        if (!sb_add_ident(&b, d->cols[i].name)) return -1;
        first = 0;
        np++;
    }
    if (np == 0 || np > VMS_DML_MAX_PARAMS) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0,
                      "insert: no writable columns or too many");
        return -1;
    }
    if (!sb_add(&b, L") VALUES (")) return -1;
    first = 1;
    for (i = 0; i < d->ncols; i++) {
        int is_null = 0;
        const char* text;
        VmsValue tmp;
        if (!col_writable(&d->cols[i])) continue;
        /* NULL key columns are omitted so server-side defaults apply
         * (e.g. uniqueidentifier PK with DEFAULT NEWID()) */
        if (is_key_col(d, i)) {
            text = get(user, i, &is_null);
            if (is_null || !text || !text[0]) continue;
        }
        if (col_present && !col_present[i]) continue;
        text = get(user, i, &is_null);
        if (!text_to_value(text, is_null, d->cols[i].vtype, &tmp)) {
            vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0,
                          "insert: bad value for '%s'", d->cols[i].name);
            return -1;
        }
        if (!first && !sb_add(&b, L", ")) return -1;
        first = 0;
        if (!sb_add_value(&b, &tmp)) return -1;
    }
    if (!sb_add(&b, L")")) return -1;
    {
        long long irows = 0;
        int r = vms_conn_exec_dml(d->cn, sql, NULL, 0, NULL, NULL, 0, NULL, 0,
                                  &irows, err);
        if (rows_affected) *rows_affected = (r == 0) ? 1 : 0;
        return r;
    }
}

int vms_dml_update(VmsDmlContext* d, const unsigned char* col_present,
                   VmsDmlValueGet key_get, void* key_user,
                   VmsDmlValueGet value_get, void* user,
                   long long* rows_affected, VmsError* err)
{
    wchar_t sql[VMS_DML_SQL_CAP];
    SqlB b;
    int i, first = 1, np = 0, ok = 1;
    VmsDmlValueGet kget = key_get ? key_get : value_get;
    void* kuser = key_get ? key_user : user;
    vms_error_ok(err);
    sql[0] = 0; b.s = sql; b.cap = VMS_DML_SQL_CAP; b.len = 0;
    if (!sb_add(&b, L"UPDATE ") || !sb_add_table(&b, d->schema, d->table) ||
        !sb_add(&b, L" SET ")) return -1;
    for (i = 0; i < d->ncols; i++) {
        int is_null = 0;
        const char* text;
        VmsValue tmp;
        if (!col_writable(&d->cols[i])) continue;
        if (is_key_col(d, i)) continue;
        if (col_present && !col_present[i]) continue;
        text = value_get(user, i, &is_null);
        if (!text_to_value(text, is_null, d->cols[i].vtype, &tmp)) {
            vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0,
                          "update: bad value for '%s'", d->cols[i].name);
            return -1;
        }
        if (!first && !sb_add(&b, L", ")) return -1;
        if (!sb_add_ident(&b, d->cols[i].name)) return -1;
        if (!sb_add(&b, L" = ")) return -1;
        if (!sb_add_value(&b, &tmp)) return -1;
        first = 0;
        np++;
    }
    if (np == 0) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "update: nothing to set");
        return -1;
    }
    if (!sb_add(&b, L" WHERE ")) return -1;
    for (i = 0; i < d->key.part_count && ok; i++) {
        int col = key_part_col(d, i);
        int is_null = 0;
        const char* text;
        VmsValue tmp;
        if (col < 0) { ok = 0; break; }
        text = kget(kuser, col, &is_null);
        if (!text_to_value(text, is_null, d->cols[col].vtype, &tmp)) { ok = 0; break; }
        if (i && !sb_add(&b, L" AND ")) return -1;
        if (!sb_add_ident(&b, d->cols[col].name)) return -1;
        if (!sb_add(&b, L" = ")) return -1;
        if (!sb_add_value(&b, &tmp)) return -1;
    }
    if (!ok) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "update: bad key value");
        return -1;
    }
    {
        long long irows = 0;
        int r = vms_conn_exec_dml(d->cn, sql, NULL, 0, NULL, NULL, 0, NULL, 0,
                                  &irows, err);
        if (rows_affected) *rows_affected = (r == 0) ? 1 : 0;
        return r;
    }
}

int vms_dml_delete(VmsDmlContext* d, VmsDmlValueGet key_get, void* key_user,
                   VmsDmlValueGet value_get, void* user,
                   long long* rows_affected, VmsError* err)
{
    wchar_t sql[VMS_DML_SQL_CAP];
    SqlB b;
    int i, ok = 1;
    VmsDmlValueGet kget = key_get ? key_get : value_get;
    void* kuser = key_get ? key_user : user;

    vms_error_ok(err);
    sql[0] = 0; b.s = sql; b.cap = VMS_DML_SQL_CAP; b.len = 0;
    if (!sb_add(&b, L"DELETE FROM ") || !sb_add_table(&b, d->schema, d->table) ||
        !sb_add(&b, L" WHERE ")) return -1;
    for (i = 0; i < d->key.part_count && ok; i++) {
        int col = key_part_col(d, i);
        int is_null = 0;
        const char* text;
        VmsValue tmp;
        if (col < 0) return -1;
        text = kget(kuser, col, &is_null);
        if (!text_to_value(text, is_null, d->cols[col].vtype, &tmp)) { ok = 0; break; }
        if (i && !sb_add(&b, L" AND ")) return -1;
        if (!sb_add_ident(&b, d->cols[col].name)) return -1;
        if (!sb_add(&b, L" = ")) return -1;
        if (!sb_add_value(&b, &tmp)) return -1;
    }
    if (!ok) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "delete: bad key value");
        return -1;
    }
    {
        long long irows = 0;
        int r = vms_conn_exec_dml(d->cn, sql, NULL, 0, NULL, NULL, 0, NULL, 0,
                                  &irows, err);
        if (rows_affected) *rows_affected = (r == 0) ? 1 : 0;
        return r;
    }
}
