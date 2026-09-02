/* vms_error.h — error model of the client layer (R3).
 * VmsError carries an ODBC-diagnostic snapshot plus a portable class tag;
 * classification happens once, at capture time, inside the adapter layer. */
#ifndef VIRTUALMSSQL_VMS_ERROR_H
#define VIRTUALMSSQL_VMS_ERROR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum VmsErrClass {
    VMS_OK = 0,
    VMS_ERR_DRIVER_NOT_FOUND,   /* no ODBC Driver 18 registered */
    VMS_ERR_CONNECT,            /* login/connect failed, transport alive-unknown */
    VMS_ERR_TRANSPORT,          /* connection broken mid-use -> quarantine */
    VMS_ERR_TIMEOUT,            /* HYT00 family */
    VMS_ERR_CANCELLED,          /* HY008 after explicit cancel */
    VMS_ERR_EXEC,               /* statement-level SQL failure */
    VMS_ERR_QUARANTINED,        /* op rejected: connection retired */
    VMS_ERR_NO_MEMORY,
    VMS_ERR_INVALID_ARG,
    VMS_ERR_INTERNAL            /* adapter invariant violated */
    /* the full 19-class model lands with R11+; enum is extensible */
} VmsErrClass;

typedef struct VmsError {
    VmsErrClass cls;
    char sqlstate[6];
    int native;
    char message[1024];
} VmsError;

/* clear + set helpers (implementation in the adapter) */
void vms_error_ok(VmsError* e);
void vms_error_set(VmsError* e, VmsErrClass cls, const char* sqlstate,
                   int native, const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* VIRTUALMSSQL_VMS_ERROR_H */
