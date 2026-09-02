/* vms_foundation.h — checked-arithmetic, bounded-buffer and conversion
 * primitives shared by the extension (R2 foundation core).
 *
 * Everything here is allocation-free except the explicitly marked heap
 * helpers, so the layer is safe to call from any error path. */
#ifndef VIRTUALMSSQL_VMS_FOUNDATION_H
#define VIRTUALMSSQL_VMS_FOUNDATION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- checked arithmetic (R2) ----
 * All return 0 on overflow, 1 on success with *out filled. */
int vms_add_sz(size_t a, size_t b, size_t* out);
int vms_mul_sz(size_t a, size_t b, size_t* out);
int vms_add_i64(int64_t a, int64_t b, int64_t* out);
int vms_mul_i64(int64_t a, int64_t b, int64_t* out);

/* ---- fault allocator hook (R2) ----
 * Test-only injection point: when installed and it returns nonzero, the next
 * bounded-buffer allocation fails. The shipping extension never installs it. */
void vms_set_alloc_fail_hook(int (*fn)(void));

/* ---- bounded byte buffers ---- */
typedef struct VmsBounded {
    char*    data;   /* heap-owned; NULL when capacity is 0 */
    size_t   cap;    /* total allocation size */
    size_t   len;    /* used bytes, excluding the terminator */
} VmsBounded;

/* Allocate a bounded buffer of exactly cap bytes (cap+1 reserved internally
 * for the terminator; cap==0 is valid and produces an empty buffer). */
int vms_buf_init(VmsBounded* b, size_t cap);

/* Release and zero the buffer. Safe to call twice. */
void vms_buf_free(VmsBounded* b);

/* Append len bytes from src. Returns 0 when the append does not fit. */
int vms_buf_append(VmsBounded* b, const void* src, size_t len);

/* Current used length in bytes. */
size_t vms_buf_len(const VmsBounded* b);

/* ---- UTF-8 <-> UTF-16 conversion (wide = UTF-16LE on Windows) ----
 * Convert with explicit bounds; never reads or writes past the given sizes.
 * out_len receives the number of wchar_t/code units written. Returns 0 on
 * success, -1 when the destination is too small, -2 on invalid encoding. */
int vms_utf8_to_utf16(const char* src, size_t src_bytes,
                      wchar_t* dst, size_t dst_wchars, size_t* out_wchars);
int vms_utf16_to_utf8(const wchar_t* src, size_t src_wchars,
                      char* dst, size_t dst_bytes, size_t* out_bytes);

/* ---- versioned fingerprints (R2 diagnostics) ----
 * Stable 64-bit FNV-1a fingerprint of a byte range, salted with a stage tag
 * so different subsystems never collide accidentally. */
uint64_t vms_fingerprint(const void* data, size_t len, const char* stage);

#ifdef __cplusplus
}
#endif

#endif /* VIRTUALMSSQL_VMS_FOUNDATION_H */
