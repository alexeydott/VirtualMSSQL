/* vms_odbc_worker.c — connection-affine worker (R3).
 *
 * One worker per connection. All ODBC calls happen here. The job that owns
 * the connection's current statement publishes the HSTMT before the blocking
 * call and unpublishes after; vms_worker_cancel_active() targets that handle
 * with SQLCancelHandle(SQL_HANDLE_STMT), which R0 proved effective while
 * SQLCancelHandle(SQL_HANDLE_DBC) is not. */
#include "vms_odbc_worker.h"
#include <sql.h>
#include <sqlext.h>
#include <process.h>
#include <stdlib.h>

struct VmsJob {
    VmsJobFn fn;
    void* arg;
    volatile LONG done;
    struct VmsJob* next;
};

struct VmsWorker {
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv_push;
    CONDITION_VARIABLE cv_done;
    struct VmsJob* head;
    struct VmsJob* tail;
    volatile LONG stopping;
    HANDLE thread;
    /* published active statement for cross-thread attention */
    volatile SQLHSTMT active_stmt;
    volatile LONG in_job;
};

static unsigned __stdcall worker_main(void* arg)
{
    VmsWorker* w = (VmsWorker*)arg;
    for (;;) {
        struct VmsJob* j = NULL;
        EnterCriticalSection(&w->cs);
        while (!w->head && !w->stopping) {
            SleepConditionVariableCS(&w->cv_push, &w->cs, INFINITE);
        }
        if (w->head) {
            j = w->head;
            w->head = j->next;
            if (!w->head) w->tail = NULL;
        } else if (w->stopping) {
            LeaveCriticalSection(&w->cs);
            break;
        }
        LeaveCriticalSection(&w->cs);

        if (j) {
            InterlockedExchange(&w->in_job, 1);
            j->fn(j->arg);
            InterlockedExchange(&w->in_job, 0);
            /* signal under the lock: prevents lost wake-up against the
             * checker in vms_worker_run (classic cv race) */
            EnterCriticalSection(&w->cs);
            InterlockedExchange(&j->done, 1);
            WakeConditionVariable(&w->cv_done);
            LeaveCriticalSection(&w->cs);
        }
    }
    return 0;
}

VmsWorker* vms_worker_start(void)
{
    VmsWorker* w = (VmsWorker*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(VmsWorker));
    if (!w) return NULL;
    InitializeCriticalSection(&w->cs);
    InitializeConditionVariable(&w->cv_push);
    InitializeConditionVariable(&w->cv_done);
    w->thread = (HANDLE)_beginthreadex(NULL, 0, worker_main, w, 0, NULL);
    if (!w->thread) {
        DeleteCriticalSection(&w->cs);
        HeapFree(GetProcessHeap(), 0, w);
        return NULL;
    }
    return w;
}

static void worker_push(VmsWorker* w, struct VmsJob* j)
{
    j->next = NULL;
    j->done = 0;
    EnterCriticalSection(&w->cs);
    if (w->tail) w->tail->next = j;
    else w->head = j;
    w->tail = j;
    LeaveCriticalSection(&w->cs);
    WakeConditionVariable(&w->cv_push);
}

void vms_worker_run(VmsWorker* w, VmsJobFn fn, void* arg)
{
    struct VmsJob j;
    j.fn = fn;
    j.arg = arg;
    j.next = NULL;
    j.done = 0;
    worker_push(w, &j);
    /* wait for completion (stack job: must not return early) */
    EnterCriticalSection(&w->cs);
    while (!j.done) {
        SleepConditionVariableCS(&w->cv_done, &w->cs, INFINITE);
    }
    LeaveCriticalSection(&w->cs);
}

struct VmsJob* vms_worker_post(VmsWorker* w, VmsJobFn fn, void* arg)
{
    struct VmsJob* j = (struct VmsJob*)HeapAlloc(GetProcessHeap(), 0, sizeof(struct VmsJob));
    if (!j) return NULL;
    j->fn = fn;
    j->arg = arg;
    worker_push(w, j);
    return (VmsJob*)j;
}

void vms_job_wait(VmsJob* j)
{
    VmsWorker* w;
    if (!j) return;
    /* jobs complete in order; poll done flag (set before cv wake) */
    while (!InterlockedCompareExchange(&j->done, 0, 0)) {
        Sleep(1);
    }
    (void)w;
    HeapFree(GetProcessHeap(), 0, j);
}

void vms_worker_cancel_active(VmsWorker* w)
{
    SQLHSTMT st;
    if (!w) return;
    st = (SQLHSTMT)InterlockedCompareExchangePointer((volatile PVOID*)&w->active_stmt, NULL, NULL);
    if (st) {
        SQLCancelHandle(SQL_HANDLE_STMT, st);
    }
}

void vms_worker_set_active(VmsWorker* w, void* stmt_handle)
{
    if (!w) return;
    InterlockedExchangePointer((volatile PVOID*)&w->active_stmt, (PVOID)stmt_handle);
}

void vms_worker_stop(VmsWorker* w)
{
    if (!w) return;
    EnterCriticalSection(&w->cs);
    w->stopping = 1;
    LeaveCriticalSection(&w->cs);
    WakeConditionVariable(&w->cv_push);
    WaitForSingleObject(w->thread, INFINITE);
    CloseHandle(w->thread);
    DeleteCriticalSection(&w->cs);
    HeapFree(GetProcessHeap(), 0, w);
}
