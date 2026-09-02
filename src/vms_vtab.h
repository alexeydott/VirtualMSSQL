/* vms_vtab.h — SQLite virtual table module over the client runtime (R6).
 *
 * CREATE VIRTUAL TABLE x USING virtualmssql(
 *   schema = 'dbo',
 *   table  = 'mytable',
 *   conn   = 'profile-key'      -- reserved; R7 will wire profiles
 * );
 *
 * R6 delivers the read-only base: full scans, streaming, LOB, independent
 * cursors. Pushdown arrives with the R7 planner. */
#ifndef VIRTUALMSSQL_VMS_VTAB_H
#define VIRTUALMSSQL_VMS_VTAB_H

#include "vms_client.h"
#include "vms_connstr.h"
#include "vms_pool.h"
#include "sqlite3ext.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VmsVtabEnv VmsVtabEnv;

/* create the module environment: a pool + the active profile. The module
 * registers as "virtualmssql". Returns NULL on failure (err filled). */
VmsVtabEnv* vms_vtab_env_create(const VmsProfile* profile, VmsError* err);
void vms_vtab_env_destroy(VmsVtabEnv* env);

/* register the eponymous module on a host connection. */
int vms_vtab_register(sqlite3* db, VmsVtabEnv* env, char** pzErrMsg);

#ifdef __cplusplus
}
#endif

#endif /* VIRTUALMSSQL_VMS_VTAB_H */
