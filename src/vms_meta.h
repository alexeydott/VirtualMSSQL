/* vms_meta.h — metadata / type registry / stable identity (R5).
 *
 * All information comes from the SQL Server catalog (sys.schemas,
 * sys.objects, sys.columns, sys.types, sys.indexes, sys.index_columns,
 * sys.computed_columns, sys.triggers). Every query uses validated,
 * quote-escaped identifiers — callers cannot inject SQL through names. */
#ifndef VIRTUALMSSQL_VMS_META_H
#define VIRTUALMSSQL_VMS_META_H

#include "vms_client.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VMS_META_MAX_COLUMNS 512
#define VMS_META_MAX_KEY_PARTS 16
#define VMS_META_MAX_NAME 128

/* ---- identifiers ---- */
/* 1 when name matches [A-Za-z0-9_#@][$] and fits the buffer. */
int vms_meta_ident_valid(const char* name, size_t max_chars);

/* N'...' quoted literal into out (UTF-8 buffer); doubles inner quotes. */
int vms_meta_quote_ident(const char* name, char* out, size_t cap);

/* ---- table presence / kind ---- */
typedef enum VmsObjKind {
    VMS_OBJ_ABSENT = 0,
    VMS_OBJ_TABLE,
    VMS_OBJ_VIEW
} VmsObjKind;

VmsObjKind vms_meta_object_kind(VmsConnection* cn, const char* schema,
                                const char* name, VmsError* err);

/* ---- columns (type registry view of sys.columns x sys.types) ---- */
typedef struct VmsMetaColumn {
    char name[VMS_META_MAX_NAME];
    char type_name[64];       /* sys.types name, e.g. int, nvarchar, decimal */
    VmsColType vtype;         /* registry mapping (lossless decode plan) */
    int is_nullable;
    int is_identity;
    int is_computed;
    unsigned long max_length;
    unsigned char precision;
    unsigned char scale;
} VmsMetaColumn;

typedef struct VmsTableColumns {
    VmsMetaColumn cols[VMS_META_MAX_COLUMNS];
    int count;
} VmsTableColumns;

/* 1 on success (count may be 0 when the object has no columns read). */
int vms_meta_columns(VmsConnection* cn, const char* schema, const char* name,
                     VmsTableColumns* out, VmsError* err);

/* ---- stable identity key ---- */
typedef struct VmsKeyColumn {
    char name[VMS_META_MAX_NAME];
    VmsColType vtype;
} VmsKeyColumn;

typedef struct VmsStableKey {
    char index_name[VMS_META_MAX_NAME];
    int is_primary_key;
    int part_count;
    VmsKeyColumn parts[VMS_META_MAX_KEY_PARTS];
} VmsStableKey;

/* selection order: PRIMARY KEY first (any index kind), then a suitable
 * UNIQUE index. Unsuitable keys are skipped:
 *   - any nullable key column
 *   - any computed key column
 *   - filtered (has_filter), disabled or hypothetical indexes
 *   - key wider than VMS_META_MAX_KEY_PARTS
 * Returns 1 when a stable key exists. */
int vms_meta_stable_key(VmsConnection* cn, const char* schema,
                        const char* name, VmsStableKey* out, VmsError* err);

/* ---- versioned lossless identity token ----
 * v1 text format: "v1|<kind><len>:<payload>|..." — hex payloads make the
 * token byte-exact and safe to store in TEXT columns and logs are safe
 * because tokens reveal no values (they are opaque identities). */
#define VMS_IDENTITY_TOKEN_MAX 1024

int vms_identity_encode(const VmsValue* parts, int nparts,
                        char* out, size_t cap);
/* decode into caller-provided parts storage; *out_nparts receives count.
 * Decoded text/blob parts are HeapAlloc'd; free with vms_identity_free. */
int vms_identity_decode(const char* token, VmsValue* parts, int max_parts,
                        int* out_nparts);
void vms_identity_free(VmsValue* parts, int nparts);

/* ---- triggers (presence matters for write-path planning in R10) ---- */
typedef struct VmsTriggerList {
    char names[16][VMS_META_MAX_NAME];
    char events[16][32];  /* e.g. "INSERT", "UPDATE", "DELETE" */
    int count;
} VmsTriggerList;

int vms_meta_triggers(VmsConnection* cn, const char* schema,
                      const char* table, VmsTriggerList* out, VmsError* err);

#ifdef __cplusplus
}
#endif

#endif /* VIRTUALMSSQL_VMS_META_H */
