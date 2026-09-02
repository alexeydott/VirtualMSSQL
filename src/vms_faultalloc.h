/* vms_faultalloc.h — fault-injecting allocator for OOM-path tests (R2).
 * Only test binaries link this; the shipping extension never does. */
#ifndef VIRTUALMSSQL_VMS_FAULTALLOC_H
#define VIRTUALMSSQL_VMS_FAULTALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Arm the injector: fail the first allocation after `countdown` successful
 * ones (countdown==0 → fail immediately). Then restore on disarm. */
void vms_fault_alloc_arm(long countdown);
void vms_fault_alloc_disarm(void);

/* heap wrappers honoring the injector; use everywhere in code under test */
void* vms_fault_malloc(size_t n);
void  vms_fault_free(void* p);

#ifdef __cplusplus
}
#endif

#endif /* VIRTUALMSSQL_VMS_FAULTALLOC_H */
