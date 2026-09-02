/* vms_credentials.c — credential_ref resolution + built-in providers (R4).
 *
 * ABI contract: a provider must set abi_version == VMS_CRED_PROVIDER_ABI_VERSION
 * and wipe secret buffers in secret_end. The dispatcher refuses providers
 * with unknown ABI versions instead of guessing the layout. */
#include "vms_credentials.h"
#include <windows.h>
#include <wincred.h>
#include <string.h>
#include <stdlib.h>

/* ---- dispatcher ---- */

static const VmsCredProviderV1* g_provider = NULL;

int vms_cred_provider_valid(const VmsCredProviderV1* p)
{
    return p && p->abi_version == VMS_CRED_PROVIDER_ABI_VERSION &&
           p->name && p->secret_begin && p->secret_end;
}

void vms_cred_set_provider(const VmsCredProviderV1* p)
{
    g_provider = p;
}

int vms_cred_secret_begin(const VmsCredentialRef* ref,
                          wchar_t** secret, size_t* secret_len, VmsError* err)
{
    static wchar_t buf[512];
    size_t n = 0;
    vms_error_ok(err);
    if (!g_provider || !ref || !secret || !secret_len) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "credential ref: bad args/provider");
        return 0;
    }
    if (!g_provider->secret_begin(g_provider->ctx, ref->key, buf, 512, &n, err)) {
        return 0;
    }
    *secret = buf;
    *secret_len = n;
    return 1;
}

void vms_cred_secret_end(const VmsCredentialRef* ref, wchar_t* secret, size_t secret_len)
{
    if (g_provider && secret) {
        g_provider->secret_end(g_provider->ctx, secret, 512);
    }
    (void)ref;
    (void)secret_len;
}

/* ---- in-memory provider (tests) ---- */

typedef struct MemEntry {
    wchar_t* key;
    wchar_t* secret;
    struct MemEntry* next;
} MemEntry;

static MemEntry* g_mem_entries = NULL;
static VmsCredProviderV1 g_memory_provider = {
    VMS_CRED_PROVIDER_ABI_VERSION, "memory", NULL, NULL, NULL
};

int vms_cred_memory_set(const wchar_t* key, const wchar_t* secret)
{
    MemEntry* e;
    size_t klen = wcslen(key) + 1;
    size_t slen = wcslen(secret) + 1;
    for (e = g_mem_entries; e; e = e->next) {
        if (!wcscmp(e->key, key)) {
            SecureZeroMemory(e->secret, wcslen(e->secret) * sizeof(wchar_t));
            HeapFree(GetProcessHeap(), 0, e->secret);
            e->secret = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, slen * sizeof(wchar_t));
            if (!e->secret) return 0;
            memcpy(e->secret, secret, slen * sizeof(wchar_t));
            return 1;
        }
    }
    e = (MemEntry*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(MemEntry));
    if (!e) return 0;
    e->key = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, klen * sizeof(wchar_t));
    e->secret = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, slen * sizeof(wchar_t));
    if (!e->key || !e->secret) {
        if (e->key) HeapFree(GetProcessHeap(), 0, e->key);
        if (e->secret) HeapFree(GetProcessHeap(), 0, e->secret);
        HeapFree(GetProcessHeap(), 0, e);
        return 0;
    }
    memcpy(e->key, key, klen * sizeof(wchar_t));
    memcpy(e->secret, secret, slen * sizeof(wchar_t));
    e->next = g_mem_entries;
    g_mem_entries = e;
    return 1;
}

static int mem_secret_begin(void* ctx, const wchar_t* key,
                            wchar_t* buf, size_t max_len, size_t* secret_len,
                            VmsError* err)
{
    MemEntry* e;
    size_t n;
    (void)ctx;
    vms_error_ok(err);
    for (e = g_mem_entries; e; e = e->next) {
        if (!wcscmp(e->key, key)) {
            n = wcslen(e->secret);
            if (n + 1 > max_len) {
                vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "secret too large");
                return 0;
            }
            memcpy(buf, e->secret, (n + 1) * sizeof(wchar_t));
            *secret_len = n;
            return 1;
        }
    }
    vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "credential key not found");
    return 0;
}

static void mem_secret_end(void* ctx, wchar_t* buf, size_t max_len)
{
    (void)ctx;
    if (buf) SecureZeroMemory(buf, max_len * sizeof(wchar_t));
}

const VmsCredProviderV1* vms_cred_memory_provider(void)
{
    g_memory_provider.secret_begin = mem_secret_begin;
    g_memory_provider.secret_end = mem_secret_end;
    g_memory_provider.ctx = NULL;
    return &g_memory_provider;
}

/* ---- Windows Credential Manager provider ---- */

#define VMS_WCRED_PREFIX L"VirtualMSSQL/"

static int wcred_secret_begin(void* ctx, const wchar_t* key,
                              wchar_t* buf, size_t max_len, size_t* secret_len,
                              VmsError* err)
{
    wchar_t full_name[300];
    PCREDENTIALW cred = NULL;
    size_t n;
    (void)ctx;
    vms_error_ok(err);

    if (wcslen(key) + wcslen(VMS_WCRED_PREFIX) + 1 > sizeof(full_name) / sizeof(full_name[0])) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "credential key too long");
        return 0;
    }
    _snwprintf_s(full_name, sizeof(full_name) / sizeof(full_name[0]), _TRUNCATE,
                 L"%s%s", VMS_WCRED_PREFIX, key);

    if (!CredReadW(full_name, CRED_TYPE_GENERIC, 0, &cred)) {
        DWORD e = GetLastError();
        if (e == ERROR_NOT_FOUND) {
            vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0,
                          "credential '%ls' not found in Windows Credential Manager", key);
        } else {
            vms_error_set(err, VMS_ERR_INTERNAL, NULL, 0,
                          "CredRead failed (winerror=%lu)", (unsigned long)e);
        }
        return 0;
    }
    /* generic credential blob is our UTF-16 secret */
    n = cred->CredentialBlobSize / sizeof(wchar_t);
    if (n + 1 > max_len) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "secret too large for buffer");
        CredFree(cred);
        return 0;
    }
    memcpy(buf, cred->CredentialBlob, n * sizeof(wchar_t));
    buf[n] = 0;
    /* tolerate a stored terminator inside the blob */
    if (n > 0 && buf[n - 1] == 0) {
        n--;
        buf[n] = 0;
    }
    *secret_len = n;
    CredFree(cred);
    return 1;
}

static void wcred_secret_end(void* ctx, wchar_t* buf, size_t max_len)
{
    (void)ctx;
    if (buf) SecureZeroMemory(buf, max_len * sizeof(wchar_t));
}

static VmsCredProviderV1 g_wincred_provider = {
    VMS_CRED_PROVIDER_ABI_VERSION, "wincred", wcred_secret_begin, wcred_secret_end, NULL
};

const VmsCredProviderV1* vms_cred_wincred_provider(void)
{
    return &g_wincred_provider;
}
