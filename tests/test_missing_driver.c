/* R1.4 CTest suite 4: missing-driver diagnostic class.
 * The adapter probe must return a definitive answer and the diagnostic class
 * must be DRIVER_NOT_FOUND when absent. This does NOT require the driver to
 * be installed — it verifies the diagnostic contract of the probe. */
#include "vms_internal.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    int present = vms_driver18_present();
    const char* cls = vms_error_class_driver_probe(present);

    if (present) {
        printf("driver 18 present; class=%s\n", cls);
    } else {
        printf("driver 18 absent; class=%s\n", cls);
    }
    if (strcmp(cls, "DRIVER_NOT_FOUND") != 0 && strcmp(cls, "OK") != 0) {
        fprintf(stderr, "unexpected diagnostic class: %s\n", cls);
        return 1;
    }
    /* class must be consistent with the probe */
    if ((present && strcmp(cls, "OK") != 0) || (!present && strcmp(cls, "DRIVER_NOT_FOUND") != 0)) {
        fprintf(stderr, "class/probe mismatch: present=%d class=%s\n", present, cls);
        return 1;
    }
    printf("test_missing_driver: PASS\n");
    return 0;
}
