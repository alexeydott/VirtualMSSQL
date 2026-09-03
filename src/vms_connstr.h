/* vms_connstr.h — connection profile and strict connection-string builder (R4).
 *
 * The builder emits ONLY keys from the strict grammar. Forbidden keys
 * (Retry_*, DSN, FileDSN, SaveFile, implicit-pool toggles) are structurally
 * impossible to inject: caller input never becomes raw key=value pairs. */
#ifndef VIRTUALMSSQL_VMS_CONNSTR_H
#define VIRTUALMSSQL_VMS_CONNSTR_H

#include "vms_error.h"
#include "vms_credentials.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum VmsAuthMode {
    VMS_AUTH_SQL = 0,
    VMS_AUTH_WINDOWS
} VmsAuthMode;

typedef enum VmsTlsMode {
    VMS_TLS_VERIFY = 0,              /* default: encrypt + validate cert */
    VMS_TLS_TRUST_SERVER_CERTIFICATE,/* encrypt, skip validation */
    VMS_TLS_OPTIONAL                 /* encrypt when server supports */
} VmsTlsMode;

typedef struct VmsProfile {
    wchar_t server[256];       /* host[\instance][:port] or host,port */
    wchar_t database[128];
    VmsAuthMode auth;
    VmsCredentialRef cred;     /* used only when auth == VMS_AUTH_SQL */
    VmsTlsMode tls;
    int login_timeout_sec;     /* >0; 0 = driver default */
    int query_timeout_sec;     /* 0 = none */
    wchar_t app_name[64];      /* ACST app name; default VirtualMSSQL */
    int mars;                  /* R11: enable MARS for the txn connection */
} VmsProfile;

/* fill defaults; server must be set by caller afterwards. */
void vms_profile_defaults(VmsProfile* p);

/* parse "server[=]host[:port][\instance];db=...;auth=sql|windows;
 * cred=key;tls=verify|trust|optional;login_timeout=N;query_timeout=N;app=NAME"
 * Keys unknown to the strict grammar are REJECTED (VMS_ERR_INVALID_ARG).
 * Returns 1 on success. */
int vms_profile_parse(const char* spec, VmsProfile* p, VmsError* err);

/* build the ODBC connection string (UTF-16, driver-18 grammar) from the
 * profile. SQL auth resolves the secret through the active credential
 * provider; the secret never appears in the returned string. Returns 1 on
 * success (caller frees *out with vms_connstr_free). */
int vms_connstr_build(const VmsProfile* p, wchar_t** out, size_t* out_len,
                      VmsError* err);
void vms_connstr_free(wchar_t* s);

#ifdef __cplusplus
}
#endif

#endif /* VIRTUALMSSQL_VMS_CONNSTR_H */
