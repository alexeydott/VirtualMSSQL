/* vms_identity.c - identity token codec + identifier validation (R5/R16).
 *
 * Split out of vms_meta.c in R16: these helpers are pure CPU code with no
 * ODBC dependency, so offline consumers (fuzz harness, offline tools) link
 * them standalone.
 */
#include "vms_meta.h"
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>


int vms_meta_ident_valid(const char* name, size_t max_chars)
{
    size_t n = 0;
    if (!name || !name[0]) return 0;
    for (; name[n]; n++) {
        unsigned char c = (unsigned char)name[n];
        if (!(isalnum(c) || c == '_' || c == '#' || c == '@' || c == '$')) return 0;
        if (n >= max_chars) return 0;
    }
    return n > 0;
}

int vms_meta_quote_ident(const char* name, char* out, size_t cap)
{
    /* N'name' with doubled inner quotes; escape defensively even though
     * callers pass validated identifiers */
    size_t o = 0, i;
    if (cap < 5) return 0;
    out[o++] = 'N';
    out[o++] = '\'';
    for (i = 0; name[i] && o + 3 < cap; i++) {
        out[o++] = name[i];
        if (name[i] == '\'') out[o++] = '\'';
    }
    if (o + 2 > cap) return 0;
    out[o++] = '\'';
    out[o] = 0;
    return 1;
}

/* ---- versioned lossless identity token ---- */

static void hex_encode(const unsigned char* src, size_t n, char* out)
{
    static const char* hx = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[i * 2] = hx[(src[i] >> 4) & 0xF];
        out[i * 2 + 1] = hx[src[i] & 0xF];
    }
    out[n * 2] = 0;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int vms_identity_encode(const VmsValue* parts, int nparts,
                        char* out, size_t cap)
{
    size_t o = 0;
    int i;
#define PUTCH(ch) do { if (o + 1 >= cap) return 0; out[o++] = (char)(ch); } while (0)
#define PUTS(s) do { size_t _n = strlen(s); \
    if (o + _n >= cap) return 0; memcpy(out + o, s, _n); o += _n; } while (0)

    PUTS("v1|");
    for (i = 0; i < nparts; i++) {
        const VmsValue* p = &parts[i];
        if (i) PUTCH('|');
        switch (p->type) {
        case VMS_VAL_NULL:
            PUTS("n0:");
            break;
        case VMS_VAL_INT64: {
            char tmp[32];
            _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "i%lld:", p->i);
            PUTS(tmp);
            break;
        }
        case VMS_VAL_FLOAT64: {
            char tmp[40];
            _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "f%.17g:", p->f);
            PUTS(tmp);
            break;
        }
        case VMS_VAL_TEXT: {
            char tmp[32];
            char* hx = (char*)HeapAlloc(GetProcessHeap(), 0, p->text_len * 2 + 1);
            if (!hx) return 0;
            hex_encode((const unsigned char*)p->text, p->text_len, hx);
            _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "s%zu:", p->text_len);
            PUTS(tmp);
            PUTS(hx);
            HeapFree(GetProcessHeap(), 0, hx);
            break;
        }
        case VMS_VAL_BLOB: {
            char tmp[32];
            char* hx = (char*)HeapAlloc(GetProcessHeap(), 0, p->blob_len * 2 + 1);
            if (!hx) return 0;
            hex_encode(p->blob, p->blob_len, hx);
            _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "b%zu:", p->blob_len);
            PUTS(tmp);
            PUTS(hx);
            HeapFree(GetProcessHeap(), 0, hx);
            break;
        }
        default:
            return 0;
        }
    }
    if (o + 1 >= cap) return 0;
    out[o] = 0;
    return 1;
#undef PUTCH
#undef PUTS
}

static char* heap_bytes(const unsigned char* src, size_t n)
{
    char* p = (char*)HeapAlloc(GetProcessHeap(), 0, n + 1);
    if (p) { memcpy(p, src, n); p[n] = 0; }
    return p;
}

int vms_identity_decode(const char* token, VmsValue* parts, int max_parts,
                        int* out_nparts)
{
    const char* p = token;
    int n = 0;
    if (!token || strncmp(token, "v1|", 3) != 0) return 0;
    p += 3;
    while (*p) {
        char kind = *p++;
        char lenbuf[16];
        size_t li = 0;
        VmsValue* v;
        if (n >= max_parts) return 0;
        memset(&parts[n], 0, sizeof(VmsValue));
        v = &parts[n];
        switch (kind) {
        case 'n':
            v->type = VMS_VAL_NULL;
            /* encode emits "n0:"; skip the length digits */
            while (*p && *p != ':') p++;
            if (*p != ':') return 0;
            p++;
            n++;
            break;
        case 'i': {
            char* end;
            long long sign = 1;
            v->type = VMS_VAL_INT64;
            if (*p == '-') { sign = -1; p++; }
            else if (*p == '+') p++;
            if (!isdigit((unsigned char)*p)) return 0;
            v->i = 0;
            while (isdigit((unsigned char)*p)) {
                v->i = v->i * 10 + (*p - '0');
                p++;
            }
            v->i *= sign;
            if (*p != ':') return 0;
            p++;
            n++;
            (void)end;
            break;
        }
        case 'f': {
            char* end;
            v->type = VMS_VAL_FLOAT64;
            v->f = strtod(p, &end);
            if (end == p || *end != ':') return 0;
            p = end + 1;
            n++;
            break;
        }
        case 's':
        case 'b': {
            size_t blen, i;
            unsigned char* raw;
            /* V781: bounds-check li before using it as lenbuf index */
            while (li < sizeof(lenbuf) - 1 && p[li] && p[li] != ':') {
                lenbuf[li] = p[li]; li++;
            }
            lenbuf[li] = 0;
            if (p[li] != ':') return 0;
            blen = (size_t)strtoul(lenbuf, NULL, 10);
            p += li + 1;
            raw = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, blen + 1);
            if (!raw) return 0;
            for (i = 0; i < blen; i++) {
                int hi = hex_digit(p[i * 2]);
                int lo = hex_digit(p[i * 2 + 1]);
                if (hi < 0 || lo < 0) { HeapFree(GetProcessHeap(), 0, raw); return 0; }
                raw[i] = (unsigned char)((hi << 4) | lo);
            }
            p += blen * 2;
            if (kind == 's') {
                v->type = VMS_VAL_TEXT;
                v->text = heap_bytes(raw, blen);
                v->text_len = blen;
                if (!v->text) { HeapFree(GetProcessHeap(), 0, raw); return 0; }
            } else {
                v->type = VMS_VAL_BLOB;
                v->blob = raw;
                v->blob_len = blen;
            }
            n++;
            break;
        }
        default:
            return 0;
        }
        if (*p == '|') p++;
        else if (*p == 0) break;
        else return 0;
    }
    *out_nparts = n;
    return 1;
}

void vms_identity_free(VmsValue* parts, int nparts)
{
    int i;
    for (i = 0; i < nparts; i++) {
        if (parts[i].text) HeapFree(GetProcessHeap(), 0, parts[i].text);
        if (parts[i].blob) HeapFree(GetProcessHeap(), 0, parts[i].blob);
        parts[i].text = NULL;
        parts[i].blob = NULL;
    }
}

/* ---- triggers ---- */

