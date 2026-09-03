/* vms_meta_cache.h — metadata cache / shadow storage (R13).
 *
 * The cache is a private SQLite database (memory or temp file) holding
 * serialized VmsTableColumns snapshots keyed by "schema.table". Each
 * snapshot carries:
 *   - the fully serialized column list (name/type/vtype/nullability/...)
 *   - a schema fingerprint (FNV-1a over the canonical serialization)
 *   - a capture timestamp (UTC, unix seconds)
 * Live mode bypasses the cache; cached mode consults it and applies the
 * live validation policy:
 *   - server reachable + fingerprint equal  -> fresh hit
 *   - server reachable + fingerprint differ -> schema drift (error)
 *   - server unreachable                    -> stale read (allowed, flagged)
 * Corrupt cache rows (bad fingerprint / truncated payload) are detected
 * on load and reported (SQLITE_ERROR), never silently trusted. */
#ifndef VIRTUALMSSQL_VMS_META_CACHE_H
#define VIRTUALMSSQL_VMS_META_CACHE_H

#include <sqlite3.h>
#include "vms_meta.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VmsMetaCache VmsMetaCache;

/* open the cache: mode "memory" or "temp" (private temp file, deleted at
 * close). Returns NULL on failure. */
VmsMetaCache* vms_meta_cache_open(int temp_mode);
void vms_meta_cache_close(VmsMetaCache* c);

/* result classification for cache lookups */
typedef enum VmsCacheResult {
    VMS_CACHE_MISS = 0,   /* no entry */
    VMS_CACHE_FRESH,      /* entry matches the live fingerprint */
    VMS_CACHE_STALE,      /* entry present but server unreachable */
    VMS_CACHE_DRIFT,      /* entry present, server reachable, mismatch */
    VMS_CACHE_CORRUPT     /* entry present but payload/fingerprint broken */
} VmsCacheResult;

/* load a cached snapshot. Returns VmsCacheResult; on FRESH/STALE the
 * columns are decoded into out (out_count receives the column count).
 * live_fp: the current fingerprint from the server (0 = server probe
 * skipped/unreachable). */
VmsCacheResult vms_meta_cache_get(VmsMetaCache* c, const char* schema,
                                  const char* table, VmsTableColumns* out,
                                  int* out_count, unsigned long long live_fp);

/* store a snapshot captured from the live server (overwrites any
 * previous entry for the key). Returns 1 on success. */
int vms_meta_cache_put(VmsMetaCache* c, const char* schema,
                       const char* table, const VmsTableColumns* cols,
                       int count, unsigned long long fp, long long utc_ts);

/* drop an entry (e.g. after a drift rejection). */
void vms_meta_cache_drop(VmsMetaCache* c, const char* schema,
                         const char* table);

/* canonical fingerprint over the column list (FNV-1a 64). Includes name,
 * order, type_name, vtype, nullability, identity/computed flags,
 * max_length/precision/scale. */
unsigned long long vms_meta_fingerprint(const VmsTableColumns* cols, int count);

/* ---- xShadowName / xIntegrity support (R13) ----
 * A vtab may declare shadow table name suffixes; SQLite routes writes to
 * <vtab>_vms_schema / <vtab>_vms_metadata to the parent module. These
 * helpers implement the naming rule and the self-check. */
int vms_meta_is_shadow_name(const char* base, const char* candidate);
/* integrity check over a shadow snapshot pair: recomputes the payload
 * fingerprint and compares it with the stored one; 1 = consistent. */
int vms_meta_shadow_check(const unsigned char* payload, int payload_len,
                          unsigned long long stored_fp);

#ifdef __cplusplus
}
#endif

#endif /* VIRTUALMSSQL_VMS_META_CACHE_H */
