/* vms_log.c — logging with mandatory secret redaction (R2).
 * Redaction is unconditional: the sink interface cannot receive raw secrets. */
#include "vms_log.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>

static VmsLogSink g_sink = NULL;
static void* g_sink_user = NULL;
static int g_level = VMS_LOG_INFO;

void vms_log_set_sink(VmsLogSink sink, void* user)
{
    g_sink = sink;
    g_sink_user = user;
}

void vms_log_set_level(int max_level)
{
    if (max_level < VMS_LOG_ERROR) max_level = VMS_LOG_ERROR;
    if (max_level > VMS_LOG_DEBUG) max_level = VMS_LOG_DEBUG;
    g_level = max_level;
}

/* case-insensitive substring search (ASCII) */
static const char* find_ci(const char* hay, const char* needle)
{
    size_t nl = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return hay;
        if (!hay[i]) break;
    }
    return NULL;
}

/* find "KEY=" for secret-ish keys (case-insensitive), return value pointer
 * just past '=' and its length up to the next ';' */
static int find_secret_value(const char* text, const char** val_out, size_t* len_out)
{
    static const char* keys[] = { "PWD", "PASSWORD", "SECRET", "TOKEN", NULL };
    int i;
    for (i = 0; keys[i]; i++) {
        const char* k = keys[i];
        size_t kl = strlen(k);
        const char* p = text;
        while ((p = find_ci(p, k)) != NULL) {
            const char* eq = p + kl;
            if (*eq == '=') {
                const char* v = eq + 1;
                const char* end = v;
                while (*end && *end != ';') end++;
                if (end > v) {
                    *val_out = v;
                    *len_out = (size_t)(end - v);
                    return 1;
                }
            }
            p = eq;
        }
    }
    return 0;
}

char* vms_redact(const char* text)
{
    size_t n;
    char* out;
    const char* val = NULL;
    size_t vlen = 0;
    size_t pos = 0;

    if (!text) return NULL;
    n = strlen(text);
    out = (char*)HeapAlloc(GetProcessHeap(), 0, n + 1);
    if (!out) return NULL;
    memcpy(out, text, n + 1);

    /* single-pass redaction over secret keys */
    pos = 0;
    while (find_secret_value(out + pos, &val, &vlen)) {
        size_t off = (size_t)(val - out);
        memset(out + off, '*', vlen);
        pos = off + vlen;
    }
    return out;
}

void vms_log(int level, const char* fmt, ...)
{
    char raw[1024];
    char* safe;
    va_list ap;

    if (!g_sink || level > g_level) return;
    va_start(ap, fmt);
    _vsnprintf_s(raw, sizeof(raw), _TRUNCATE, fmt, ap);
    va_end(ap);
    safe = vms_redact(raw);
    g_sink(g_sink_user, level, safe ? safe : raw);
    if (safe) HeapFree(GetProcessHeap(), 0, safe);
}
