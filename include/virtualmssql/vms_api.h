/* vms_api.h — public entry points of the VirtualMSSQL SQLite extension.
 * TZ 01_TZ_VirtualMSSQL: artifact virtualmssql.dll must export
 * sqlite3_virtualmssql_init and sqlite3_extension_init.
 * Final public API (vms_api.h proper) lands in R18; this header holds only
 * the load-time entry points agreed in R1. */
#ifndef VIRTUALMSSQL_VMS_API_H
#define VIRTUALMSSQL_VMS_API_H

#include "sqlite3ext.h"

#ifdef _WIN32
#  define VMS_EXPORT __declspec(dllexport)
#else
#  define VMS_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Primary entry point: SQLite resolves sqlite3_<name>_init from the DLL name. */
VMS_EXPORT int sqlite3_virtualmssql_init(sqlite3* db, char** pzErrMsg,
                                         const sqlite3_api_routines* pApi);

/* Legacy alias required by the specification. */
VMS_EXPORT int sqlite3_extension_init(sqlite3* db, char** pzErrMsg,
                                      const sqlite3_api_routines* pApi);

#ifdef __cplusplus
}
#endif

#endif /* VIRTUALMSSQL_VMS_API_H */
