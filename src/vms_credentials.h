/* vms_credentials.h — credential reference and versioned provider ABI (R4).
 *
 * A credential_ref never stores the secret inline in caller-visible memory:
 * providers materialize it only inside vms_cred_secret_begin/end scope and
 * must zero the buffer on end. Secret material is never logged (R2 redaction
 * covers accidental formatting; providers additionally avoid heap copies). */
#ifndef VIRTUALMSSQL_VMS_CREDENTIALS_H
#define VIRTUALMSSQL_VMS_CREDENTIALS_H

#include "vms_error.h"
#include <stddef.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- versioned provider ABI ---- */
#define VMS_CRED_PROVIDER_ABI_VERSION 1

typedef struct VmsCredProviderV1 VmsCredProviderV1;

struct VmsCredProviderV1 {
    unsigned abi_version;                 /* must be VMS_CRED_PROVIDER_ABI_VERSION */
    const char* name;                     /* provider id, e.g. "memory", "wincred" */

    /* open the secret for key: fills buf (max_len wchar) and secret_len.
     * Returns 0 on success. */
    int (*secret_begin)(void* ctx, const wchar_t* key,
                        wchar_t* buf, size_t max_len, size_t* secret_len,
                        VmsError* err);
    /* wipe + release the material produced by secret_begin. */
    void (*secret_end)(void* ctx, wchar_t* buf, size_t max_len);
    void* ctx;                            /* provider-private */
};

/* validate a provider before registration (ABI version check). */
int vms_cred_provider_valid(const VmsCredProviderV1* p);
/* install the active provider (thread-global; not thread-safe during init). */
void vms_cred_set_provider(const VmsCredProviderV1* p);

/* ---- credential_ref: an opaque handle into the active provider ---- */
typedef struct VmsCredentialRef {
    wchar_t key[256];      /* provider key; contains no secret material */
} VmsCredentialRef;

/* resolve a credential_ref into a transient secret buffer.
 * On success *secret is valid until vms_cred_secret_end. The buffer is
 * provider-owned; callers must not free() or copy it around. */
int vms_cred_secret_begin(const VmsCredentialRef* ref,
                          wchar_t** secret, size_t* secret_len, VmsError* err);
void vms_cred_secret_end(const VmsCredentialRef* ref,
                         wchar_t* secret, size_t secret_len);

/* ---- built-in providers ---- */
/* in-memory registry (tests / embedded use). Register keys with
 * vms_cred_memory_set; secrets are stored zero-on-free. */
const VmsCredProviderV1* vms_cred_memory_provider(void);
int vms_cred_memory_set(const wchar_t* key, const wchar_t* secret);

/* Windows Credential Manager provider (production path).
 * Reads generic credentials under the prefix L"VirtualMSSQL/". */
const VmsCredProviderV1* vms_cred_wincred_provider(void);

#ifdef __cplusplus
}
#endif

#endif /* VIRTUALMSSQL_VMS_CREDENTIALS_H */
