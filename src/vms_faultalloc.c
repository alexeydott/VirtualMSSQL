/* vms_faultalloc.c — deterministic allocation-failure injection (R2 leftover).
 * Test-only: never linked into the shipped extension. */
#include "vms_faultalloc.h"
#include <windows.h>

static volatile LONG g_armed = 0;
static volatile LONG g_countdown = 0;

void vms_fault_alloc_arm(long countdown)
{
    InterlockedExchange(&g_countdown, countdown);
    InterlockedExchange(&g_armed, 1);
}

void vms_fault_alloc_disarm(void)
{
    InterlockedExchange(&g_armed, 0);
}

void* vms_fault_malloc(size_t n)
{
    if (InterlockedCompareExchange(&g_armed, 0, 0)) {
        /* count down; fail at zero */
        LONG left = InterlockedDecrement(&g_countdown);
        if (left < 0) {
            return NULL;
        }
    }
    return HeapAlloc(GetProcessHeap(), 0, n);
}

void vms_fault_free(void* p)
{
    if (p) HeapFree(GetProcessHeap(), 0, p);
}
