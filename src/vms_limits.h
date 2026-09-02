/* vms_limits.h — resource-limit framework (R2).
 * Every limit can be lowered at runtime by the embedder; raising above the
 * default requires an explicit opt-in API that the host does not have in 1.0. */
#ifndef VIRTUALMSSQL_VMS_LIMITS_H
#define VIRTUALMSSQL_VMS_LIMITS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum VmsLimit {
    VMS_LIMIT_RESULT_ROWS = 0,
    VMS_LIMIT_RESULT_BYTES,
    VMS_LIMIT_ROW_BYTES,
    VMS_LIMIT_COLUMN_BYTES,
    VMS_LIMIT_QUERY_BYTES,
    VMS_LIMIT_PARAMETER_BYTES,
    VMS_LIMIT_PARAMETERS,
    VMS_LIMIT_IDENTITY_BYTES,
    VMS_LIMIT_COLUMNS,
    VMS_LIMIT_IN_ITEMS,
    VMS_LIMIT_COUNT
} VmsLimit;

/* Defaults informed by R0 probe results (parameter ceiling 1999). */
#define VMS_LIMIT_DEFAULT_RESULT_ROWS      1000000ULL
#define VMS_LIMIT_DEFAULT_RESULT_BYTES     268435456ULL  /* 256 MiB */
#define VMS_LIMIT_DEFAULT_ROW_BYTES        1048576ULL    /* 1 MiB */
#define VMS_LIMIT_DEFAULT_COLUMN_BYTES     1048576ULL    /* 1 MiB */
#define VMS_LIMIT_DEFAULT_QUERY_BYTES      1048576ULL    /* 1 MiB */
#define VMS_LIMIT_DEFAULT_PARAMETER_BYTES  16777216ULL   /* 16 MiB */
#define VMS_LIMIT_DEFAULT_PARAMETERS       1999ULL       /* R0 measured ceiling */
#define VMS_LIMIT_DEFAULT_IDENTITY_BYTES   4096ULL
#define VMS_LIMIT_DEFAULT_COLUMNS          512ULL
#define VMS_LIMIT_DEFAULT_IN_ITEMS         1024ULL

/* Set every limit to its default. */
void vms_limits_reset(void);

/* Read current limit value. */
uint64_t vms_limits_get(int limit);

/* Lower a limit; raising above the current value is rejected (returns 0).
 * Unknown ids are rejected as well. */
int vms_limits_set(int limit, uint64_t value);

/* Convenience predicate: value fits under the limit. */
int vms_limits_ok(int limit, uint64_t value);

#ifdef __cplusplus
}
#endif

#endif /* VIRTUALMSSQL_VMS_LIMITS_H */
