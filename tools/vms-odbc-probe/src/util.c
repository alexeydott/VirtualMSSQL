#include "probe.h"
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

static FILE* g_log = NULL;

void logf_ctx(ProbeCtx* ctx, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    if (!g_log && ctx && ctx->cfg.log_path) {
        g_log = fopen(ctx->cfg.log_path, "wb");
    }
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
}

const char* status_name(ProbeStatus s)
{
    switch (s) {
    case PROBE_PASS: return "PASS";
    case PROBE_FAIL: return "FAIL";
    case PROBE_SKIP: return "SKIP";
    }
    return "?";
}

bool parse_ll(const char* s, long long* out)
{
    char* end = NULL;
    if (!s || !*s) return false;
    *out = _strtoll_l(s, &end, 10, NULL);
    if (!end || *end != '\0') return false;
    return true;
}
