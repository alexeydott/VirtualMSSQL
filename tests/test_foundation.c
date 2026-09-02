/* R2 gate suite: checked arithmetic, bounded buffers, UTF conversion,
 * limits, fingerprints, log redaction. */
#include "vms_foundation.h"
#include "vms_limits.h"
#include "vms_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { g_fail++; fprintf(stderr, "FAIL@%d: %s\n", __LINE__, #cond); } \
} while (0)

static char g_last_log[2048];
static void log_capture(void* user, int level, const char* line)
{
    (void)user; (void)level;
    strncpy_s(g_last_log, sizeof(g_last_log), line, _TRUNCATE);
}

int main(void)
{
    size_t sz;
    int64_t i64;
    VmsBounded b;
    wchar_t w16[64];
    char u8[64];
    size_t n;

    /* ---- checked arithmetic ---- */
    sz = SIZE_MAX;
    CHECK(vms_add_sz(SIZE_MAX - 1, 1, &sz) && sz == SIZE_MAX);
    if (vms_add_sz(SIZE_MAX, 1, &sz)) { g_fail++; fprintf(stderr, "add_sz overflow not caught\n"); }
    if (vms_mul_sz(SIZE_MAX, 2, &sz)) { g_fail++; fprintf(stderr, "mul_sz overflow not caught\n"); }
    /* 0x10000 * 0x10000 fits only in 64-bit size_t */
    if (sizeof(size_t) >= 8) {
        CHECK(vms_mul_sz(0x10000, 0x10000, &sz) && sz == 0x100000000ULL);
    } else {
        /* on 32-bit this product overflows and must be refused */
        if (vms_mul_sz(0x10000, 0x10000, &sz)) {
            g_fail++; fprintf(stderr, "32-bit mul_sz overflow not caught\n");
        }
    }
    CHECK(vms_add_i64(INT64_MAX - 1, 1, &i64) && i64 == INT64_MAX);
    if (vms_add_i64(INT64_MAX, 1, &i64)) { g_fail++; fprintf(stderr, "add_i64 overflow not caught\n"); }
    if (vms_mul_i64(INT64_MAX, 2, &i64)) { g_fail++; fprintf(stderr, "mul_i64 overflow not caught\n"); }
    if (vms_mul_i64(INT64_MIN, -1, &i64)) { g_fail++; fprintf(stderr, "mul_i64 INT64_MIN*-1 not caught\n"); }

    /* ---- bounded buffer ---- */
    {
        VmsBounded b;
        CHECK(vms_buf_init(&b, 10));
        CHECK(vms_buf_append(&b, "abc", 3));
        CHECK(vms_buf_len(&b) == 3);
        if (vms_buf_append(&b, "defghijklm", 10)) {
            g_fail++; fprintf(stderr, "append beyond cap accepted\n");
        }
        CHECK(vms_buf_append(&b, "d", 1) && vms_buf_len(&b) == 4);
        vms_buf_free(&b);
        vms_buf_free(&b); /* idempotent */
        CHECK(b.data == NULL && b.len == 0);
    }

    /* ---- UTF conversions ---- */
    /* ASCII */
    CHECK(vms_utf8_to_utf16("abc", 3, w16, 64, &n) == 0 && n == 3);
    CHECK(w16[0] == L'a' && w16[2] == L'c'); /* no terminator: contract is raw count */
    /* Cyrillic (2-byte UTF-8) */
    {
        const char* cyr = "\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
        CHECK(vms_utf8_to_utf16(cyr, 12, w16, 64, &n) == 0 && n == 6);
    }
    {
        const char* emoji = "\xF0\x9F\x99\x82"; /* U+1F642 */
        CHECK(vms_utf8_to_utf16(emoji, 4, w16, 64, &n) == 0 && n == 2);
    }
    /* destination too small */
    CHECK(vms_utf8_to_utf16("abcdef", 6, w16, 3, &n) == -1);
    /* invalid UTF-8 */
    CHECK(vms_utf8_to_utf16("\xFF\xFE", 2, w16, 64, &n) == -2);
    /* round trip */
    {
        const char* text = "mixed текст 🚀";
        size_t wlen = 0, blen = 0;
        char back[128];
        if (vms_utf8_to_utf16(text, strlen(text), w16, 64, &wlen) != 0) g_fail++;
        if (vms_utf16_to_utf8(w16, wlen, back, sizeof(back), &blen) != 0) g_fail++;
        if (blen != strlen(text) || memcmp(back, text, blen) != 0) g_fail++;
    }
    /* surrogate-pair input accepted; lone surrogate tolerated by the
     * non-strict path (WC_ERR_INVALID_CHARS proved to AV inside
     * WideCharToMultiByte for large buffers in this environment, so strict
     * validation is performed at a higher layer, not per conversion) */
    {
        wchar_t lone[2] = { 0xD83D, 0 }; /* high surrogate alone */
        if (vms_utf16_to_utf8(lone, 1, u8, sizeof(u8), &n) != 0) g_fail++;
    }

    /* ---- limits ---- */
    vms_limits_reset();
    if (!vms_limits_ok(VMS_LIMIT_PARAMETERS, 1999)) g_fail++;
    if (vms_limits_ok(VMS_LIMIT_PARAMETERS, 2000)) g_fail++;
    if (!vms_limits_set(VMS_LIMIT_PARAMETERS, 100)) g_fail++;
    if (vms_limits_ok(VMS_LIMIT_PARAMETERS, 101)) g_fail++;
    if (vms_limits_set(VMS_LIMIT_PARAMETERS, 2000)) { g_fail++; fprintf(stderr, "raise above current accepted\n"); }
    if (vms_limits_set(999, 1)) { g_fail++; fprintf(stderr, "unknown limit id accepted\n"); }

    /* ---- fingerprints ---- */
    {
        uint64_t f1 = vms_fingerprint("abc", 3, "conn");
        uint64_t f2 = vms_fingerprint("abc", 3, "conn");
        uint64_t f3 = vms_fingerprint("abc", 3, "stmt");
        uint64_t f4 = vms_fingerprint("abd", 3, "conn");
        if (f1 != f2) g_fail++;
        if (f1 == f3) g_fail++;
        if (f1 == f4) g_fail++;
    }

    /* ---- log redaction ---- */
    vms_log_set_sink(log_capture, NULL);
    g_last_log[0] = 0;
    vms_log(VMS_LOG_INFO, "connect ok; Server=x;PWD=SuperSecret!;MARS_Connection=No");
    if (strstr(g_last_log, "SuperSecret")) { g_fail++; fprintf(stderr, "secret leaked: %s\n", g_last_log); }
    if (!strstr(g_last_log, "PWD=****")) { g_fail++; fprintf(stderr, "PWD not redacted: %s\n", g_last_log); }
    if (!strstr(g_last_log, "MARS_Connection=No")) g_fail++;
    g_last_log[0] = 0;
    vms_log(VMS_LOG_INFO, "auth with password=hunter2 failed");
    if (strstr(g_last_log, "hunter2")) { g_fail++; fprintf(stderr, "password leaked: %s\n", g_last_log); }
    if (!strstr(g_last_log, "password=*******")) { g_fail++; fprintf(stderr, "password not redacted: %s\n", g_last_log); }

    vms_log_set_sink(NULL, NULL);

    if (g_fail == 0) {
        printf("test_foundation: PASS\n");
        return 0;
    }
    fprintf(stderr, "test_foundation: %d failures\n", g_fail);
    return 1;
}
