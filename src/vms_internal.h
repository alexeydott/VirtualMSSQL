/* vms_internal.h — shared internals of the extension (R1 stub stage). */
#ifndef VIRTUALMSSQL_VMS_INTERNAL_H
#define VIRTUALMSSQL_VMS_INTERNAL_H

#include "sqlite3.h"

#define VMS_PROJECT_NAME "virtualmssql"
#define VMS_PROJECT_VERSION "0.1.0-r1"
/* SQLite baseline per TZ: 3.44.0 */
#define VMS_SQLITE_BASELINE 3044000

/* capability check result codes */
#define VMS_CAP_OK 0
#define VMS_CAP_SQLITE_TOO_OLD 1

/* Capability + version gate (R1.3). Pure function so it can be unit-tested
 * against simulated host versions. api may be NULL when only the version is
 * checked. Returns VMS_CAP_OK or a VMS_CAP_* code. */
int vms_check_host_capabilities(int sqlite_version_number);

/* Human-readable diagnostic for a failed capability check. */
const char* vms_capability_diagnostic(int cap_code, int sqlite_version_number);

/* ODBC adapter layer (R1 stub): driver 18 presence probe.
 * True when "ODBC Driver 18 for SQL Server" is registered with the
 * Driver Manager. Never throws; result feeds the DRIVER_NOT_FOUND
 * diagnostic class at connect time (R4+). */
int vms_driver18_present(void);

/* Diagnostic class for a failed driver probe (VMS error model, R2 defines the
 * full 19-class model; this is the R1 stub of it). */
const char* vms_error_class_driver_probe(int driver_found);

#endif /* VIRTUALMSSQL_VMS_INTERNAL_H */
