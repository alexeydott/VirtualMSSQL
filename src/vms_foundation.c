/* vms_foundation.c — checked arithmetic, bounded buffers, UTF conversion,
 * fingerprints (R2 foundation core). */
#include "vms_foundation.h"
#include <windows.h>
#include <limits.h>
#include <string.h>

/* ---- checked arithmetic ---- */
int vms_add_sz(size_t a, size_t b, size_t* out)
{
    if (a > SIZE_MAX - b) return 0;
    *out = a + b;
    return 1;
}

int vms_mul_sz(size_t a, size_t b, size_t* out)
{
    if (a != 0 && b > SIZE_MAX / a) return 0;
    *out = a * b;
    return 1;
}

int vms_add_i64(int64_t a, int64_t b, int64_t* out)
{
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return 0;
    *out = a + b;
    return 1;
}

int vms_mul_i64(int64_t a, int64_t b, int64_t* out)
{
    if (a == 0 || b == 0) { *out = 0; return 1; }
    if (a == -1 && b == INT64_MIN) return 0;
    if (b == -1 && a == INT64_MIN) return 0;
    if (a > INT64_MAX / b || a < INT64_MIN / b) return 0;
    *out = a * b;
    return 1;
}

/* ---- fault allocator hook ---- */
static int (*g_alloc_fail_hook)(void) = NULL;

void vms_set_alloc_fail_hook(int (*fn)(void))
{
    g_alloc_fail_hook = fn;
}

/* ---- bounded buffers ---- */
int vms_buf_init(VmsBounded* b, size_t cap)
{
    b->data = NULL;
    b->cap = 0;
    b->len = 0;
    if (cap == 0) return 1;
    /* reserve cap bytes + terminator; overflow-checked */
    {
        size_t total;
        if (!vms_add_sz(cap, 1, &total)) return 0;
        if (g_alloc_fail_hook && g_alloc_fail_hook()) return 0;
        b->data = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total);
        if (!b->data) return 0;
        b->cap = cap;
    }
    return 1;
}

void vms_buf_free(VmsBounded* b)
{
    if (b->data) {
        SecureZeroMemory(b->data, b->cap);
        HeapFree(GetProcessHeap(), 0, b->data);
        b->data = NULL;
    }
    b->cap = 0;
    b->len = 0;
}

int vms_buf_append(VmsBounded* b, const void* src, size_t len)
{
    size_t need;
    if (!b->data && b->cap > 0) return 0;
    if (!vms_add_sz(b->len, len, &need)) return 0;
    if (need > b->cap) return 0;
    memcpy(b->data + b->len, src, len);
    b->len = need;
    b->data[b->len] = 0;
    return 1;
}

size_t vms_buf_len(const VmsBounded* b)
{
    return b->len;
}

/* ---- UTF conversion ---- */
int vms_utf8_to_utf16(const char* src, size_t src_bytes,
                      wchar_t* dst, size_t dst_wchars, size_t* out_wchars)
{
    int need;
    if (src_bytes > INT_MAX) return -1;
    need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                               src, (int)src_bytes, NULL, 0);
    if (need <= 0) return -2;
    if ((size_t)need > dst_wchars && dst != NULL) return -1;
    if (dst) {
        int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          src, (int)src_bytes, dst, need);
        if (written <= 0) return -2;
    }
    if (out_wchars) *out_wchars = (size_t)need;
    return 0;
}

int vms_utf16_to_utf8(const wchar_t* src, size_t src_wchars,
                      char* dst, size_t dst_bytes, size_t* out_bytes)
{
    int need;
    if (src_wchars > INT32_MAX) return -1;
    need = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                               src, (int)src_wchars, NULL, 0, NULL, NULL);
    if (need <= 0) return -2;
    /* dst==NULL is a pure size probe: report the requirement, fail nothing */
    if (dst == NULL) {
        if (out_bytes) *out_bytes = (size_t)need;
        return 0;
    }
    if ((size_t)need > dst_bytes) return -1;
    {
        int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                          src, (int)src_wchars,
                                          dst, need, NULL, NULL);
        if (written <= 0) return -2;
        if (out_bytes) *out_bytes = (size_t)written;
    }
    return 0;
}

/* ---- fingerprints ---- */
uint64_t vms_fingerprint(const void* data, size_t len, const char* stage)
{
    const unsigned char* p = (const unsigned char*)data;
    uint64_t h = 1469598103934665603ULL; /* FNV-1a 64 offset basis */
    const char* s;
    size_t i;

    for (s = stage; s && *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}
