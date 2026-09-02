/* R1.3/R1.4 — host capability checks and diagnostics (stub stage).
 * No silent downgrade: missing capability aborts load with explicit message. */
#include "vms_internal.h"
#include <stdio.h>

int vms_check_host_capabilities(int sqlite_version_number)
{
    if (sqlite_version_number < VMS_SQLITE_BASELINE) {
        return VMS_CAP_SQLITE_TOO_OLD;
    }
    return VMS_CAP_OK;
}

const char* vms_capability_diagnostic(int cap_code, int sqlite_version_number)
{
    static char msg[160];
    switch (cap_code) {
    case VMS_CAP_OK:
        return "ok";
    case VMS_CAP_SQLITE_TOO_OLD:
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
            "virtualmssql: host SQLite version %d.%d.%d is below the required "
            "baseline 3.44.0; refusing to load (no silent downgrade)",
            sqlite_version_number / 1000000,
            (sqlite_version_number / 1000) % 1000,
            sqlite_version_number % 1000);
        return msg;
    default:
        return "virtualmssql: unknown capability-check failure";
    }
}
