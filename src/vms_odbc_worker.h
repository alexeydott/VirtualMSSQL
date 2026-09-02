/* vms_odbc_worker.h — connection-affine worker thread (R3).
 * Serializes every ODBC call for one connection; publishes the active
 * statement handle so another thread can deliver attention via
 * SQLCancelHandle(SQL_HANDLE_STMT) — the only scheme proven in R0. */
#ifndef VIRTUALMSSQL_VMS_ODBC_WORKER_H
#define VIRTUALMSSQL_VMS_ODBC_WORKER_H

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VmsWorker VmsWorker;

typedef void (*VmsJobFn)(void* arg);

VmsWorker* vms_worker_start(void);
/* run fn(arg) on the worker and wait for completion */
void vms_worker_run(VmsWorker* w, VmsJobFn fn, void* arg);
/* post fn(arg); caller waits later with vms_job_wait */
typedef struct VmsJob VmsJob;
VmsJob* vms_worker_post(VmsWorker* w, VmsJobFn fn, void* arg);
void vms_job_wait(VmsJob* j);
/* deliver attention to the currently executing ODBC call (any thread) */
void vms_worker_cancel_active(VmsWorker* w);
/* worker-side bookkeeping: publish/unpublish the statement a blocking call
 * is about to use (the job runs on the worker thread) */
void vms_worker_set_active(VmsWorker* w, void* stmt_handle);
void vms_worker_stop(VmsWorker* w);

#ifdef __cplusplus
}
#endif

#endif /* VIRTUALMSSQL_VMS_ODBC_WORKER_H */
