#include "probe.h"
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>

void case_add(ProbeCtx* ctx, const char* group, const char* name,
              ProbeStatus status, const char* detail_fmt, ...)
{
    ProbeCase* c;
    va_list ap;

    if (ctx->case_count >= PROBE_MAX_CASES) return;
    c = &ctx->cases[ctx->case_count++];
    memset(c, 0, sizeof(*c));
    c->group = group;
    c->name = name;
    c->status = status;
    diag_reset(&c->diag);
    if (detail_fmt) {
        va_start(ap, detail_fmt);
        _vsnprintf_s(c->detail, sizeof(c->detail), _TRUNCATE, detail_fmt, ap);
        va_end(ap);
    }
    logf_ctx(ctx, "[%s] %s/%s%s%s", status_name(status), group, name,
             c->detail[0] ? ": " : "", c->detail[0] ? c->detail : "");
}

void case_set_status(ProbeCtx* ctx, int index, ProbeStatus status, const char* detail_fmt, ...)
{
    ProbeCase* c;
    va_list ap;
    if (index < 0 || index >= ctx->case_count) return;
    c = &ctx->cases[index];
    c->status = status;
    if (detail_fmt) {
        va_start(ap, detail_fmt);
        _vsnprintf_s(c->detail, sizeof(c->detail), _TRUNCATE, detail_fmt, ap);
        va_end(ap);
    }
    logf_ctx(ctx, "[%s] %s/%s%s%s", status_name(status), c->group, c->name,
             c->detail[0] ? ": " : "", c->detail[0] ? c->detail : "");
}

static void json_escape(const char* in, char* out, size_t outsz)
{
    size_t o = 0;
    for (; *in && o + 6 < outsz; in++) {
        unsigned char ch = (unsigned char)*in;
        if (ch == '"' || ch == '\\') {
            out[o++] = '\\';
            out[o++] = (char)ch;
        } else if (ch < 0x20) {
            o += (size_t)_snprintf_s(out + o, outsz - o, _TRUNCATE, "\\u%04x", ch);
        } else {
            out[o++] = (char)ch;
        }
    }
    out[o] = 0;
}

void report_json(ProbeCtx* ctx)
{
    FILE* f = NULL;
    int i;
    int pass = 0, fail = 0, skip = 0;

    for (i = 0; i < ctx->case_count; i++) {
        if (ctx->cases[i].status == PROBE_PASS) pass++;
        else if (ctx->cases[i].status == PROBE_FAIL) fail++;
        else skip++;
    }

    if (fopen_s(&f, ctx->cfg.json_path ? ctx->cfg.json_path : "probe-results.json", "wb") != 0 || !f) {
        logf_ctx(ctx, "report: cannot open JSON output");
        return;
    }
    fprintf(f, "{\n  \"driver18_present\": %s,\n", ctx->driver_present ? "true" : "false");
    fprintf(f, "  \"summary\": {\"pass\": %d, \"fail\": %d, \"skip\": %d},\n", pass, fail, skip);
    fprintf(f, "  \"cases\": [\n");
    for (i = 0; i < ctx->case_count; i++) {
        ProbeCase* c = &ctx->cases[i];
        char esc[4096];
        fprintf(f, "    {\"group\": ");
        json_escape(c->group, esc, sizeof(esc));
        fprintf(f, "\"%s\", \"name\": ", esc);
        json_escape(c->name, esc, sizeof(esc));
        fprintf(f, "\"%s\", \"status\": \"%s\", ", esc, status_name(c->status));
        json_escape(c->detail, esc, sizeof(esc));
        fprintf(f, "\"detail\": \"%s\"", esc);
        if (c->diag.present) {
            json_escape(c->diag.sqlstate, esc, sizeof(esc));
            fprintf(f, ", \"sqlstate\": \"%s\"", esc);
            fprintf(f, ", \"native\": %d", c->diag.native);
            json_escape(c->diag.message, esc, sizeof(esc));
            fprintf(f, ", \"message\": \"%s\"", esc);
        }
        fprintf(f, "}%s\n", i + 1 < ctx->case_count ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    logf_ctx(ctx, "report: JSON written");
}

void report_summary(ProbeCtx* ctx)
{
    int i;
    int pass = 0, fail = 0, skip = 0;
    logf_ctx(ctx, "==== SUMMARY ====");
    for (i = 0; i < ctx->case_count; i++) {
        if (ctx->cases[i].status == PROBE_PASS) pass++;
        else if (ctx->cases[i].status == PROBE_FAIL) fail++;
        else skip++;
    }
    logf_ctx(ctx, "PASS=%d FAIL=%d SKIP=%d TOTAL=%d", pass, fail, skip, ctx->case_count);
    for (i = 0; i < ctx->case_count; i++) {
        ProbeCase* c = &ctx->cases[i];
        if (c->status == PROBE_FAIL) {
            logf_ctx(ctx, "  FAIL %s/%s: %s", c->group, c->name, c->detail);
            if (c->diag.present) {
                logf_ctx(ctx, "       %s (%d): %s", c->diag.sqlstate, c->diag.native, c->diag.message);
            }
        }
    }
}
