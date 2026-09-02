/* vms_limits.c — resource-limit framework (R2). */
#include "vms_limits.h"

static uint64_t g_limits[VMS_LIMIT_COUNT] = { 0 };

static const uint64_t k_defaults[VMS_LIMIT_COUNT] = {
    VMS_LIMIT_DEFAULT_RESULT_ROWS,
    VMS_LIMIT_DEFAULT_RESULT_BYTES,
    VMS_LIMIT_DEFAULT_ROW_BYTES,
    VMS_LIMIT_DEFAULT_COLUMN_BYTES,
    VMS_LIMIT_DEFAULT_QUERY_BYTES,
    VMS_LIMIT_DEFAULT_PARAMETER_BYTES,
    VMS_LIMIT_DEFAULT_PARAMETERS,
    VMS_LIMIT_DEFAULT_IDENTITY_BYTES,
    VMS_LIMIT_DEFAULT_COLUMNS,
    VMS_LIMIT_DEFAULT_IN_ITEMS
};

void vms_limits_reset(void)
{
    int i;
    for (i = 0; i < VMS_LIMIT_COUNT; i++) g_limits[i] = k_defaults[i];
}

uint64_t vms_limits_get(int limit)
{
    if (limit < 0 || limit >= VMS_LIMIT_COUNT) return 0;
    return g_limits[limit];
}

int vms_limits_set(int limit, uint64_t value)
{
    if (limit < 0 || limit >= VMS_LIMIT_COUNT) return 0;
    if (value > g_limits[limit]) return 0; /* lowering only */
    g_limits[limit] = value;
    return 1;
}

int vms_limits_ok(int limit, uint64_t value)
{
    return value <= vms_limits_get(limit);
}
