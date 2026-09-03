/* vms_meta_cache.c — metadata cache / shadow storage (R13).
 *
 * Compiled with SQLITE_CORE like vms_mat: uses the linked host sqlite3
 * directly for the private cache db. */
#include "vms_meta_cache.h"
#include <sqlite3.h>
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define VMS_MC_SCHEMA_VERSION 1

struct VmsMetaCache {
    sqlite3* db;
    char db_path[512];
    int temp_file;
};

/* ---- canonical serialization + fingerprint ----
 * Format (deterministic, little-endian):
 *   u32 magic 'VMSC'  u32 version  u32 count
 *   per column: name \0  type_name \0  u32 vtype  u32 flags(nullable|
 *   identity|computed<<2)  u32 max_length  u8 precision  u8 scale
 * The fingerprint is FNV-1a 64 over the payload bytes. */
#define VMS_MC_MAGIC 0x43534D56u /* 'VMSC' LE */
#define VMS_MC_FLAG_NULLABLE 1u
#define VMS_MC_FLAG_IDENTITY 2u
#define VMS_MC_FLAG_COMPUTED 4u

static void put_u32(unsigned char** p, unsigned int v)
{
    (*p)[0] = (unsigned char)(v & 0xFF);
    (*p)[1] = (unsigned char)((v >> 8) & 0xFF);
    (*p)[2] = (unsigned char)((v >> 16) & 0xFF);
    (*p)[3] = (unsigned char)((v >> 24) & 0xFF);
    *p += 4;
}

static unsigned int get_u32(const unsigned char** p, const unsigned char* end)
{
    unsigned int v;
    if (end - *p < 4) return 0xFFFFFFFFu;
    v = (unsigned int)(*p)[0] | ((unsigned int)(*p)[1] << 8) |
        ((unsigned int)(*p)[2] << 16) | ((unsigned int)(*p)[3] << 24);
    *p += 4;
    return v;
}

static int payload_size(const VmsTableColumns* cols, int count)
{
    int i, sz = 12;
    if (count < 0 || count > VMS_META_MAX_COLUMNS) return -1;
    for (i = 0; i < count; i++) {
        sz += (int)strlen(cols->cols[i].name) + 1;
        sz += (int)strlen(cols->cols[i].type_name) + 1;
        sz += 4 + 4 + 4 + 1 + 1;
    }
    return sz;
}

static int payload_write(unsigned char* buf, int cap, const VmsTableColumns* cols,
                         int count)
{
    unsigned char* p = buf;
    int i;
    if (cap < 12) return 0;
    put_u32(&p, VMS_MC_MAGIC);
    put_u32(&p, VMS_MC_SCHEMA_VERSION);
    put_u32(&p, (unsigned int)count);
    for (i = 0; i < count; i++) {
        size_t n;
        unsigned int flags = 0;
        n = strlen(cols->cols[i].name) + 1;
        if (p - buf + (int)n > cap) return 0;
        memcpy(p, cols->cols[i].name, n);
        p += n;
        n = strlen(cols->cols[i].type_name) + 1;
        if (p - buf + (int)n > cap) return 0;
        memcpy(p, cols->cols[i].type_name, n);
        p += n;
        if (cols->cols[i].is_nullable) flags |= VMS_MC_FLAG_NULLABLE;
        if (cols->cols[i].is_identity) flags |= VMS_MC_FLAG_IDENTITY;
        if (cols->cols[i].is_computed) flags |= VMS_MC_FLAG_COMPUTED;
        if (p - buf + 10 > cap) return 0;
        put_u32(&p, (unsigned int)cols->cols[i].vtype);
        put_u32(&p, flags);
        put_u32(&p, cols->cols[i].max_length);
        *p++ = cols->cols[i].precision;
        *p++ = cols->cols[i].scale;
    }
    return (int)(p - buf);
}

static int payload_read(const unsigned char* buf, int len, VmsTableColumns* out,
                        int* out_count)
{
    const unsigned char* p = buf;
    const unsigned char* end = buf + len;
    unsigned int magic, version, count, i;

    if (len < 12) return 0;
    magic = get_u32(&p, end);
    version = get_u32(&p, end);
    count = get_u32(&p, end);
    if (magic != VMS_MC_MAGIC || version != VMS_MC_SCHEMA_VERSION) return 0;
    if (count == 0 || count > VMS_META_MAX_COLUMNS) return 0;
    memset(out, 0, sizeof(*out));
    for (i = 0; i < (int)count; i++) {
        VmsMetaColumn* m = &out->cols[i];
        unsigned int vtype, flags, maxlen;
        size_t slen;
        const char* s;
        /* name */
        s = (const char*)p;
        slen = strnlen_s(s, (size_t)(end - p));
        if (slen >= (size_t)(end - p) || slen == 0 || slen >= VMS_META_MAX_NAME) return 0;
        memcpy(m->name, s, slen + 1);
        p += slen + 1;
        /* type_name */
        s = (const char*)p;
        slen = strnlen_s(s, (size_t)(end - p));
        if (slen >= (size_t)(end - p) || slen == 0 || slen >= 64) return 0;
        memcpy(m->type_name, s, slen + 1);
        p += slen + 1;
        vtype = get_u32(&p, end);
        flags = get_u32(&p, end);
        maxlen = get_u32(&p, end);
        if (p + 2 > end) return 0;
        m->vtype = (VmsColType)vtype;
        if (m->vtype < VMS_CT_INT64 || m->vtype > VMS_CT_SPATIAL) return 0;
        m->is_nullable = (flags & VMS_MC_FLAG_NULLABLE) ? 1 : 0;
        m->is_identity = (flags & VMS_MC_FLAG_IDENTITY) ? 1 : 0;
        m->is_computed = (flags & VMS_MC_FLAG_COMPUTED) ? 1 : 0;
        m->max_length = maxlen;
        m->precision = *p++;
        m->scale = *p++;
        out->count++;
    }
    *out_count = out->count;
    return 1;
}

unsigned long long vms_meta_fingerprint(const VmsTableColumns* cols, int count)
{
    unsigned char stack[16384];
    unsigned char* heap = NULL;
    unsigned char* buf = stack;
    int sz = payload_size(cols, count);
    unsigned long long fp = 1469598103934665603ULL; /* FNV-1a 64 offset */
    int i;

    if (sz < 0) return 0;
    if (sz > (int)sizeof(stack)) {
        heap = (unsigned char*)malloc((size_t)sz);
        if (!heap) return 0;
        buf = heap;
    }
    if (!payload_write(buf, sz, cols, count)) {
        free(heap);
        return 0;
    }
    for (i = 0; i < sz; i++) {
        fp ^= buf[i];
        fp *= 1099511628211ULL;
    }
    free(heap);
    return fp;
}

int vms_meta_shadow_check(const unsigned char* payload, int payload_len,
                          unsigned long long stored_fp)
{
    unsigned long long fp = 1469598103934665603ULL;
    int i;
    if (!payload || payload_len < 12) return 0;
    for (i = 0; i < payload_len; i++) {
        fp ^= payload[i];
        fp *= 1099511628211ULL;
    }
    return fp == stored_fp;
}

int vms_meta_is_shadow_name(const char* base, const char* candidate)
{
    size_t bl;
    if (!base || !candidate) return 0;
    bl = strlen(base);
    if (strncmp(candidate, base, bl) != 0) return 0;
    if (!strcmp(candidate + bl, "_vms_schema")) return 1;
    if (!strcmp(candidate + bl, "_vms_metadata")) return 1;
    return 0;
}

/* ---- cache db ---- */

static int mc_exec(sqlite3* db, const char* sql)
{
    char* err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

VmsMetaCache* vms_meta_cache_open(int temp_mode)
{
    VmsMetaCache* c =
        (VmsMetaCache*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(VmsMetaCache));
    if (!c) return NULL;
    if (temp_mode) {
        char tmpdir[400];
        char name[96];
        GetTempPathA(sizeof(tmpdir), tmpdir);
        _snprintf_s(name, sizeof(name), _TRUNCATE, "vms_mdc_%lu_%u.db",
                    (unsigned long)GetCurrentProcessId(), (unsigned)GetTickCount());
        _snprintf_s(c->db_path, sizeof(c->db_path), _TRUNCATE, "%s%s", tmpdir, name);
        DeleteFileA(c->db_path);
        c->temp_file = 1;
    } else {
        strcpy_s(c->db_path, sizeof(c->db_path), ":memory:");
    }
    if (sqlite3_open(c->db_path, &c->db) != SQLITE_OK) {
        if (c->db) sqlite3_close(c->db);
        if (c->temp_file) DeleteFileA(c->db_path);
        HeapFree(GetProcessHeap(), 0, c);
        return NULL;
    }
    sqlite3_exec(c->db, "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;",
                 NULL, NULL, NULL);
    if (!mc_exec(c->db,
                 "CREATE TABLE IF NOT EXISTS vms_meta_cache("
                 " key TEXT PRIMARY KEY,"
                 " fp INTEGER NOT NULL,"
                 " captured_utc INTEGER NOT NULL,"
                 " payload BLOB NOT NULL)")) {
        vms_meta_cache_close(c);
        return NULL;
    }
    return c;
}

void vms_meta_cache_close(VmsMetaCache* c)
{
    if (!c) return;
    if (c->db) sqlite3_close(c->db);
    if (c->temp_file && c->db_path[0]) DeleteFileA(c->db_path);
    HeapFree(GetProcessHeap(), 0, c);
}

static void make_key(char* key, size_t cap, const char* schema, const char* table)
{
    _snprintf_s(key, cap, _TRUNCATE, "%s.%s", schema, table);
}

VmsCacheResult vms_meta_cache_get(VmsMetaCache* c, const char* schema,
                                  const char* table, VmsTableColumns* out,
                                  int* out_count, unsigned long long live_fp)
{
    sqlite3_stmt* st = NULL;
    char key[300];
    VmsCacheResult result = VMS_CACHE_MISS;
    const void* payload = NULL;
    int payload_len = 0;
    unsigned long long stored_fp = 0;

    if (!c || !c->db || !schema || !table) return VMS_CACHE_MISS;
    make_key(key, sizeof(key), schema, table);
    if (sqlite3_prepare_v2(c->db,
                           "SELECT fp, payload FROM vms_meta_cache WHERE key = ?1",
                           -1, &st, NULL) != SQLITE_OK)
        return VMS_CACHE_MISS;
    if (sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC) != SQLITE_OK) {
        sqlite3_finalize(st);
        return VMS_CACHE_MISS;
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
        stored_fp = (unsigned long long)sqlite3_column_int64(st, 0);
        payload = sqlite3_column_blob(st, 1);
        payload_len = sqlite3_column_bytes(st, 1);
    }
    if (!payload || payload_len <= 0) {
        sqlite3_finalize(st);
        return VMS_CACHE_MISS;
    }
    {
        /* copy payload out before finalize (the cache db owns the bytes) */
        unsigned char* copy = (unsigned char*)malloc((size_t)payload_len);
        if (!copy) {
            sqlite3_finalize(st);
            return VMS_CACHE_CORRUPT;
        }
        memcpy(copy, payload, (size_t)payload_len);
        sqlite3_finalize(st);

        if (!vms_meta_shadow_check(copy, payload_len, stored_fp)) {
            /* payload/fingerprint mismatch: the cache row is corrupt */
            free(copy);
            return VMS_CACHE_CORRUPT;
        }
        if (!payload_read(copy, payload_len, out, out_count)) {
            free(copy);
            return VMS_CACHE_CORRUPT;
        }
        free(copy);
        if (live_fp == 0)
            result = VMS_CACHE_STALE; /* probe skipped / server unreachable */
        else if (live_fp == stored_fp)
            result = VMS_CACHE_FRESH;
        else
            result = VMS_CACHE_DRIFT;
    }
    return result;
}

int vms_meta_cache_put(VmsMetaCache* c, const char* schema, const char* table,
                       const VmsTableColumns* cols, int count,
                       unsigned long long fp, long long utc_ts)
{
    sqlite3_stmt* st = NULL;
    char key[300];
    unsigned char stack[16384];
    unsigned char* heap = NULL;
    unsigned char* buf = stack;
    int sz;
    int ok = 0;

    if (!c || !c->db) return 0;
    sz = payload_size(cols, count);
    if (sz <= 0) return 0;
    if (sz > (int)sizeof(stack)) {
        heap = (unsigned char*)malloc((size_t)sz);
        if (!heap) return 0;
        buf = heap;
    }
    if (!payload_write(buf, sz, cols, count)) {
        free(heap);
        return 0;
    }
    make_key(key, sizeof(key), schema, table);
    if (sqlite3_prepare_v2(c->db,
                           "INSERT OR REPLACE INTO vms_meta_cache"
                           " (key, fp, captured_utc, payload) VALUES (?1, ?2, ?3, ?4)",
                           -1, &st, NULL) == SQLITE_OK &&
        sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_int64(st, 2, (sqlite3_int64)fp) == SQLITE_OK &&
        sqlite3_bind_int64(st, 3, utc_ts) == SQLITE_OK &&
        sqlite3_bind_blob(st, 4, buf, sz, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_DONE) {
        ok = 1;
    }
    if (st) sqlite3_finalize(st);
    free(heap);
    return ok;
}

void vms_meta_cache_drop(VmsMetaCache* c, const char* schema, const char* table)
{
    sqlite3_stmt* st = NULL;
    char key[300];
    if (!c || !c->db) return;
    make_key(key, sizeof(key), schema, table);
    if (sqlite3_prepare_v2(c->db, "DELETE FROM vms_meta_cache WHERE key = ?1",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
}
