/* vms_api.h — public entry points of the VirtualMSSQL SQLite extension.
 * TZ 01_TZ_VirtualMSSQL: artifact virtualmssql.dll must export
 * sqlite3_virtualmssql_init and sqlite3_extension_init.
 *
 * R18 public ABI (minimum per roadmap): api_version, credential-provider
 * registration, query-profile-provider registration, the Windows
 * Credential Manager provider instance, and cancellation. All functions
 * are thread-safe unless noted; errors are returned as nonzero codes, the
 * extension never raises from these entry points. */
#ifndef VIRTUALMSSQL_VMS_API_H
#define VIRTUALMSSQL_VMS_API_H

#include "sqlite3ext.h"

#ifdef _WIN32
#  define VMS_EXPORT __declspec(dllexport)
#else
#  define VMS_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Primary entry point: SQLite resolves sqlite3_<name>_init from the DLL name. */
VMS_EXPORT int sqlite3_virtualmssql_init(sqlite3* db, char** pzErrMsg,
                                         const sqlite3_api_routines* pApi);

/* Legacy alias required by the specification. */
VMS_EXPORT int sqlite3_extension_init(sqlite3* db, char** pzErrMsg,
                                      const sqlite3_api_routines* pApi);

/* ---- R18 public ABI (v1) ---- */
#define VIRTUALMSSQL_API_VERSION 1
#define VMS_QPROFILE_PROVIDER_ABI_VERSION 1

/* Returns VIRTUALMSSQL_API_VERSION of the loaded DLL. */
VMS_EXPORT int virtualmssql_api_version(void);

/* Register a credential provider. provider_v1 must point to a
 * VmsCredProviderV1 struct (see src/vms_credentials.h; abi_version must
 * equal VMS_CRED_PROVIDER_ABI_VERSION). The DLL does not take ownership:
 * the provider must outlive all connections that may resolve secrets.
 * Returns 0 on success, nonzero on invalid provider (deterministic). */
VMS_EXPORT int virtualmssql_register_credential_provider(const void* provider_v1);

/* Query-profile provider (v1): resolves a profile key (the conn='key'
 * vtab argument) into a full profile spec string. Registered providers
 * are consulted when a virtual table declares conn='key'. */
typedef struct VmsQueryProfileProviderV1 {
    unsigned abi_version;                 /* must be VMS_QPROFILE_PROVIDER_ABI_VERSION */
    const char* name;                     /* provider id for diagnostics */
    /* resolve key into out (NUL-terminated, cap chars). Returns 0 when the
     * key is known, nonzero when unknown (deterministic rejection). */
    int (*get_profile)(void* ctx, const char* key,
                       char* out, size_t cap);
    void* ctx;
} VmsQueryProfileProviderV1;

/* Register a query-profile provider (ownership stays with the host).
 * Returns 0 on success, nonzero on invalid provider. */
VMS_EXPORT int virtualmssql_register_query_profile_provider(const void* provider_v1);

/* Built-in Windows Credential Manager provider instance (reads
 * VirtualMSSQL/<key> entries via CredReadW). Never NULL on Windows. */
VMS_EXPORT const void* virtualmssql_wincred_provider(void);

/* Cancel all in-flight remote operations of this process (attention via
 * SQLCancelHandle on every live connection) and interrupt the given SQLite
 * VM. Returns the number of remote connections signaled. Thread-safe. */
VMS_EXPORT int virtualmssql_cancel(sqlite3* db);

#ifdef __cplusplus
}
#endif

#endif /* VIRTUALMSSQL_VMS_API_H */
