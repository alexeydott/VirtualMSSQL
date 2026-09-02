/* vms_pool.c — bounded connection pool (R4).
 *
 * Reuse rule: a connection returns to the pool only after clean-state
 * verification (not quarantined, @@TRANCOUNT == 0, SELECT 1 round-trip).
 * Failed checks close the connection permanently. The pool owns one
 * VmsClient (ODBC environment); host Driver Manager pooling is untouched. */
#include "vms_pool.h"
#include <windows.h>

typedef struct PoolSlot {
    VmsConnection* cn;
    struct PoolSlot* next;
} PoolSlot;

struct VmsPool {
    CRITICAL_SECTION cs;
    int capacity;
    int idle_count;
    int live_count;
    PoolSlot* idle;
    VmsClient* client;
};

VmsPool* vms_pool_create(int capacity)
{
    VmsPool* pool;
    VmsError err;
    if (capacity < 1) return NULL;
    pool = (VmsPool*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(VmsPool));
    if (!pool) return NULL;
    InitializeCriticalSection(&pool->cs);
    pool->capacity = capacity;
    pool->client = vms_client_init(&err);
    if (!pool->client) {
        DeleteCriticalSection(&pool->cs);
        HeapFree(GetProcessHeap(), 0, pool);
        return NULL;
    }
    return pool;
}

void vms_pool_destroy(VmsPool* pool)
{
    PoolSlot* s;
    if (!pool) return;
    EnterCriticalSection(&pool->cs);
    s = pool->idle;
    while (s) {
        PoolSlot* next = s->next;
        vms_conn_close(s->cn);
        HeapFree(GetProcessHeap(), 0, s);
        s = next;
    }
    pool->idle = NULL;
    pool->idle_count = 0;
    LeaveCriticalSection(&pool->cs);
    if (pool->client) vms_client_destroy(pool->client);
    DeleteCriticalSection(&pool->cs);
    HeapFree(GetProcessHeap(), 0, pool);
}

int vms_pool_idle_count(const VmsPool* pool)
{
    return pool ? pool->idle_count : 0;
}

int vms_pool_live_count(const VmsPool* pool)
{
    return pool ? pool->live_count : 0;
}

VmsConnection* vms_pool_acquire(VmsPool* pool, const VmsProfile* profile,
                                VmsError* err)
{
    PoolSlot* slot = NULL;
    VmsConnection* cn = NULL;

    if (!pool || !profile) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "pool acquire: bad args");
        return NULL;
    }

    /* 1) take an idle connection */
    EnterCriticalSection(&pool->cs);
    if (pool->idle) {
        slot = pool->idle;
        pool->idle = slot->next;
        pool->idle_count--;
    }
    LeaveCriticalSection(&pool->cs);

    /* 2) clean-state verification before reuse (TZ rule) */
    if (slot) {
        cn = slot->cn;
        HeapFree(GetProcessHeap(), 0, slot);
        if (vms_conn_verify(cn)) {
            return cn;
        }
        vms_conn_close(cn);
        EnterCriticalSection(&pool->cs);
        pool->live_count--;
        LeaveCriticalSection(&pool->cs);
        cn = NULL;
    }

    /* 3) open a fresh connection via the pool's own environment */
    {
        wchar_t* connstr = NULL;
        size_t connstr_len = 0;
        VmsError verr;
        if (!vms_connstr_build(profile, &connstr, &connstr_len, &verr)) {
            *err = verr;
            return NULL;
        }
        cn = vms_conn_open(pool->client, connstr, &verr);
        vms_connstr_free(connstr);
        if (!cn) {
            *err = verr;
            return NULL;
        }
        EnterCriticalSection(&pool->cs);
        pool->live_count++;
        LeaveCriticalSection(&pool->cs);
        return cn;
    }
}

void vms_pool_release(VmsPool* pool, VmsConnection* cn)
{
    int keep = 0;

    if (!pool || !cn) return;

    /* clean-state check before returning to the pool */
    if (vms_conn_verify(cn)) {
        EnterCriticalSection(&pool->cs);
        if (pool->idle_count < pool->capacity) {
            PoolSlot* slot = (PoolSlot*)HeapAlloc(GetProcessHeap(), 0, sizeof(PoolSlot));
            if (slot) {
                slot->cn = cn;
                slot->next = pool->idle;
                pool->idle = slot;
                pool->idle_count++;
                keep = 1;
            }
        }
        LeaveCriticalSection(&pool->cs);
    }

    if (!keep) {
        /* dirty, dead, or pool full: close permanently */
        vms_conn_close(cn);
        EnterCriticalSection(&pool->cs);
        pool->live_count--;
        LeaveCriticalSection(&pool->cs);
    }
}
