/* R1.4 CTest suite 3: capability gate.
 * The baseline version must be accepted; older simulated hosts refused. */
#include "vms_internal.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    /* current host must pass (3.53.4 host via third_party sqlite3.h) */
    if (vms_check_host_capabilities(3053004) != VMS_CAP_OK) {
        fprintf(stderr, "host sqlite 3.53.4 rejected by capability gate\n");
        return 1;
    }
    /* simulated too-old host must be refused */
    if (vms_check_host_capabilities(3039000) != VMS_CAP_SQLITE_TOO_OLD) {
        fprintf(stderr, "simulated 3.39 host was not refused\n");
        return 1;
    }
    /* baseline exact must pass */
    if (vms_check_host_capabilities(VMS_SQLITE_BASELINE) != VMS_CAP_OK) {
        fprintf(stderr, "exact baseline 3.44.0 refused (off-by-one)\n");
        return 1;
    }
    /* diagnostic must name the baseline */
    {
        const char* d = vms_capability_diagnostic(VMS_CAP_SQLITE_TOO_OLD, 3039000);
        if (!d || !strstr(d, "3.44.0")) {
            fprintf(stderr, "diagnostic does not mention baseline: %s\n", d ? d : "(null)");
            return 1;
        }
    }
    printf("test_capability: PASS\n");
    return 0;
}
