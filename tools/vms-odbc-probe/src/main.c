/* vms-odbc-probe — R0 architecture feasibility probe (TZ 02_ROADMAP R0, G0) */
#include "probe.h"
#include <string.h>
#include <stdlib.h>

static void usage(void)
{
    fprintf(stderr,
        "vms-odbc-probe — VirtualMSSQL R0 ODBC feasibility probe\n\n"
        "Usage:\n"
        "  vms-odbc-probe --server HOST[\\INSTANCE][:PORT] [--database DB]\n"
        "      [--sql-user U --sql-password P | --windows-auth]\n"
        "      [--login-timeout SEC] [--query-timeout SEC]\n"
        "      [--extra-connstr \"K=V;...\"]\n"
        "      [--only GROUP] [--stream-rows N] [--param-ceiling-max N]\n"
        "      [--json PATH] [--log PATH]\n\n"
        "Groups: connect settings exec stream cancel tx\n");
}

int main(int argc, char** argv)
{
    /* ~1.7 MB of case storage; too large for the default 1 MB thread stack */
    static ProbeCtx ctx;
    int i, run = 1;
    int exit_code;

    memset(&ctx, 0, sizeof(ctx));
    ctx.env = SQL_NULL_HENV;
    ctx.cfg.login_timeout = 10;
    ctx.cfg.query_timeout = 0;
    ctx.cfg.stream_rows = 1000000;
    ctx.cfg.json_path = "probe-results.json";
    ctx.cfg.log_path = "probe.log";

    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
#define NEXT() ((i + 1 < argc) ? argv[++i] : NULL)
        if (strcmp(a, "--server") == 0) ctx.cfg.server = NEXT();
        else if (strcmp(a, "--database") == 0) ctx.cfg.database = NEXT();
        else if (strcmp(a, "--sql-user") == 0) ctx.cfg.sql_user = NEXT();
        else if (strcmp(a, "--sql-password") == 0) ctx.cfg.sql_password = NEXT();
        else if (strcmp(a, "--windows-auth") == 0) { /* marker; auth chosen per case */ }
        else if (strcmp(a, "--login-timeout") == 0) { const char* v = NEXT(); long long n = 0; if (v && parse_ll(v, &n)) ctx.cfg.login_timeout = (int)n; }
        else if (strcmp(a, "--query-timeout") == 0) { const char* v = NEXT(); long long n = 0; if (v && parse_ll(v, &n)) ctx.cfg.query_timeout = (int)n; }
        else if (strcmp(a, "--extra-connstr") == 0) ctx.cfg.extra_connstr = NEXT();
        else if (strcmp(a, "--only") == 0) ctx.cfg.only_group = NEXT();
        else if (strcmp(a, "--stream-rows") == 0) { const char* v = NEXT(); long long n = 0; if (v) parse_ll(v, &n); ctx.cfg.stream_rows = n; }
        else if (strcmp(a, "--param-ceiling-max") == 0) { const char* v = NEXT(); long long n = 0; if (v) parse_ll(v, &n); ctx.cfg.param_ceiling_max = n; }
        else if (strcmp(a, "--json") == 0) ctx.cfg.json_path = NEXT();
        else if (strcmp(a, "--log") == 0) ctx.cfg.log_path = NEXT();
        else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) { usage(); return 0; }
        else { fprintf(stderr, "unknown argument: %s\n", a); usage(); return 2; }
#undef NEXT
    }

    if (!ctx.cfg.server || !ctx.cfg.server[0]) {
        fprintf(stderr, "error: --server is required\n");
        usage();
        return 2;
    }

    logf_ctx(&ctx, "vms-odbc-probe: starting; server=%s", ctx.cfg.server);

    if (!env_create(&ctx)) {
        logf_ctx(&ctx, "FATAL: cannot create ODBC 3.8 environment");
        return 3;
    }
    driver_detect(&ctx);
    if (!ctx.driver_present) {
        case_add(&ctx, "env", "driver18_present", PROBE_FAIL,
                 "ODBC Driver 18 for SQL Server not found (see driver list in log)");
        run = 0;
    } else {
        case_add(&ctx, "env", "driver18_present", PROBE_PASS, "ODBC Driver 18 detected");
    }

    if (run) {
        const char* g = ctx.cfg.only_group;
        if (!g || !strcmp(g, "connect")) cases_connect(&ctx);
        if (!g || !strcmp(g, "exec")) cases_exec(&ctx);
        if (!g || !strcmp(g, "stream")) cases_stream(&ctx);
        if (!g || !strcmp(g, "cancel")) cases_cancel(&ctx);
        if (!g || !strcmp(g, "tx")) cases_tx(&ctx);
    }

    report_summary(&ctx);
    report_json(&ctx);
    env_destroy(&ctx);

    exit_code = 0;
    for (i = 0; i < ctx.case_count; i++) {
        if (ctx.cases[i].status == PROBE_FAIL) exit_code = 1;
    }
    logf_ctx(&ctx, "vms-odbc-probe: exit=%d", exit_code);
    return exit_code;
}
