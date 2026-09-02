/* vms_mat.h — query materialization (R9).
 *
 * A materializer pulls a query-source snapshot into a private SQLite
 * database (mode: memory = ":memory:", temp = private temp file) and
 * publishes it atomically:
 *
 *   BUILDING  -> snapshot is being filled (rows stream from the server);
 *                external readers see nothing
 *   READY     -> all rows stored, indexes built; publish pending
 *   PUBLISHED -> readers see the snapshot (atomic switch)
 *   FAILED    -> anything failed (cancel/OOM/limit/server error):
 *                partial snapshot is discarded, never published
 *
 * The snapshot DB is private to the materializer; readers get row data
 * through vms_mat_open_reader / vms_mat_reader_*.
 *
 * NOTE: this header must be included after the consumer's sqlite3.h (or
 * sqlite3ext.h) of choice — it only uses the opaque sqlite3 type. */
#ifndef VIRTUALMSSQL_VMS_MAT_H
#define VIRTUALMSSQL_VMS_MAT_H

#include "vms_client.h"
#include "vms_query_source.h"


#ifdef __cplusplus
extern "C" {
#endif

typedef enum VmsMatMode {
    VMS_MAT_OFF = 0,     /* materialization disabled: vtab streams remotely */
    VMS_MAT_MEMORY,      /* private :memory: database */
    VMS_MAT_TEMP         /* private temp-file database */
} VmsMatMode;

typedef enum VmsMatState {
    VMS_MAT_BUILDING = 0,
    VMS_MAT_READY,
    VMS_MAT_PUBLISHED,
    VMS_MAT_FAILED
} VmsMatState;

typedef struct VmsMat VmsMat;

/* parse "off|memory|temp" (case-insensitive); -1 = unknown */
int vms_mat_mode_parse(const char* s);

VmsMat* vms_mat_create(VmsMatMode mode, long long max_rows, long long max_bytes);
void vms_mat_destroy(VmsMat* mat);

/* build the snapshot from a query source on a connection. Blocking call:
 * streams all rows. Returns 1 when the snapshot reached PUBLISHED.
 * On any failure/cancel state becomes FAILED and 0 is returned. */
int vms_mat_build(VmsMat* mat, VmsConnection* cn, const VmsQuerySource* src,
                  VmsError* err);

/* state query */
VmsMatState vms_mat_state(const VmsMat* mat);

/* reader access (valid only in PUBLISHED state) */
void* vms_mat_db(VmsMat* mat); /* sqlite3* of the private db */
/* prepared snapshot table name inside the private db */
const char* vms_mat_table_name(void);
/* number of published rows */
long long vms_mat_row_count(const VmsMat* mat);

/* request cancellation; build returns 0 and state becomes FAILED */
void vms_mat_cancel(VmsMat* mat);

#ifdef __cplusplus
}
#endif

#endif /* VIRTUALMSSQL_VMS_MAT_H */


