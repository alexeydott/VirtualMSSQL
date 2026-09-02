/* vms_pool.h — bounded connection pool with clean-state reuse policy (R4).
 *
 * Reuse rule (TZ): a connection returns to the pool only when the statement
 * is fully drained and no transaction is open; any transport error or
 * cancellation retires the connection permanently. Host Driver Manager
 * pooling is never touched. */
#ifndef VIRTUALMSSQL_VMS_POOL_H
#define VIRTUALMSSQL_VMS_POOL_H

#include "vms_client.h"
#include "vms_connstr.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VmsPool VmsPool;

/* create a bounded pool. capacity must be >= 1. */
VmsPool* vms_pool_create(int capacity);
void vms_pool_destroy(VmsPool* pool);

/* acquire: takes an idle verified connection or opens a new one.
 * The connstr comes from the strict builder (profile + provider). */
VmsConnection* vms_pool_acquire(VmsPool* pool, const VmsProfile* profile,
                                VmsError* err);

/* release: returns a connection to the pool after clean-state verification;
 * connections in a dirty/quarantined state are closed instead. */
void vms_pool_release(VmsPool* pool, VmsConnection* cn);

/* stats for tests/diagnostics */
int vms_pool_idle_count(const VmsPool* pool);
int vms_pool_live_count(const VmsPool* pool);

#ifdef __cplusplus
}
#endif

#endif /* VIRTUALMSSQL_VMS_POOL_H */
