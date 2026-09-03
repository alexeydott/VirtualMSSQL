/* vms_dml.h — write path for source=table mode=rw virtual tables (R10).
 *
 * Safety contract:
 *   - writes are allowed only for base tables with a validated stable key
 *     (vms_meta_stable_key) and mode=rw
 *   - identity, computed, rowversion/timestamp columns are never written
 *     (server-owned); rowversion may be read as an optimistic-lock token
 *   - all values reach the server as bound parameters; identifiers are
 *     bracket-quoted validated names; no value ever enters SQL text
 *   - DELETE/UPDATE always filter by the stable key columns only
 *   - optimistic_lock: UPDATE/DELETE check the rowversion token and report
 *     a conflict (0 rows affected) instead of overwriting a concurrent
 *     change */
#ifndef VIRTUALMSSQL_VMS_DML_H
#define VIRTUALMSSQL_VMS_DML_H

#include "vms_client.h"
#include "vms_meta.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VMS_DML_MAX_PARAMS 64

typedef struct VmsDmlContext {
    VmsConnection* cn;
    char schema[128];
    char table[128];
    VmsMetaColumn* cols;
    int ncols;
    VmsStableKey key;          /* validated stable key */
    int rowversion_col;        /* index of rowversion/timestamp column, -1 */
} VmsDmlContext;

/* prepare the DML context: validates the stable key and locates the
 * rowversion column (if any). Returns 1 on success. */
int vms_dml_init(VmsDmlContext* d, VmsConnection* cn, const char* schema,
                 const char* table, VmsMetaColumn* cols, int ncols,
                 VmsError* err);

/* INSERT one row: argv holds sqlite values for all vtab columns
 * (identity/computed/rowversion entries are ignored by the generator).
 * argv_get maps a vtab column index to a text value or NULL. */
typedef const char* (*VmsDmlValueGet)(void* user, int col, int* is_null);

/* generic DML exec driven by callbacks (avoids sqlite3_value in this layer):
 * col_present[i] != 0 means the column participates (INSERT: all writable;
 * UPDATE: only changed), value_get returns the UTF-8 text or NULL marker.
 * key_get returns the *pre-update* key values (argv[0] rowid / key stash);
 * when NULL, value_get is used (DELETE path where values are current). */
int vms_dml_insert(VmsDmlContext* d, const unsigned char* col_present,
                   VmsDmlValueGet value_get, void* user,
                   long long* rows_affected, VmsError* err);
int vms_dml_update(VmsDmlContext* d, const unsigned char* col_present,
                   VmsDmlValueGet key_get, void* key_user,
                   VmsDmlValueGet value_get, void* user,
                   long long* rows_affected, VmsError* err);
int vms_dml_delete(VmsDmlContext* d, VmsDmlValueGet key_get, void* key_user,
                   VmsDmlValueGet value_get, void* user,
                   long long* rows_affected, VmsError* err);

#ifdef __cplusplus
}
#endif

#endif /* VIRTUALMSSQL_VMS_DML_H */
