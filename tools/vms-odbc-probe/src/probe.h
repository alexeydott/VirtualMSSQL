#ifndef VMS_PROBE_H
#define VMS_PROBE_H

#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#include <sqlucode.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#define PROBE_MAX_CASES 512

typedef enum ProbeStatus {
    PROBE_PASS = 0,
    PROBE_FAIL,
    PROBE_SKIP
} ProbeStatus;

typedef struct ProbeDiag {
    char sqlstate[6];
    int native;
    char message[1024];
    bool present;
} ProbeDiag;

typedef struct ProbeCase {
    const char* group;
    const char* name;
    ProbeStatus status;
    char detail[2048];
    ProbeDiag diag;
    double seconds;
} ProbeCase;

typedef struct ProbeConfig {
    /* connection targets (comma-separated lists) */
    const char* server;          /* default server (host[\instance][:port]) */
    const char* database;
    const char* sql_user;
    const char* sql_password;
    int login_timeout;
    int query_timeout;
    const char* extra_connstr;
    /* run selection */
    const char* only_group;
    long long stream_rows;
    long long param_ceiling_max;
    /* output */
    const char* json_path;
    const char* log_path;
} ProbeConfig;

typedef struct ProbeCtx {
    ProbeConfig cfg;
    ProbeCase cases[PROBE_MAX_CASES];
    int case_count;
    HENV env;                    /* SQL_NULL_HENV until env created */
    char driver_version[128];
    bool driver_present;
} ProbeCtx;

/* util.c */
void logf_ctx(ProbeCtx* ctx, const char* fmt, ...);
const char* status_name(ProbeStatus s);
bool parse_ll(const char* s, long long* out);

/* diag.c */
void diag_capture(ProbeCtx* ctx, SQLSMALLINT htype, SQLHANDLE h, ProbeDiag* out);
void diag_reset(ProbeDiag* d);

/* env.c */
bool env_create(ProbeCtx* ctx);
void env_destroy(ProbeCtx* ctx);
bool driver_detect(ProbeCtx* ctx);
/* build a connection string; caller frees with free() */
wchar_t* connstr_build(ProbeCtx* ctx, const char* server, const char* auth_mode,
                       const char* tls_mode, const char* extra);
bool conn_connect(ProbeCtx* ctx, HDBC dbc, const wchar_t* wcs, ProbeDiag* diag);
void conn_close(HDBC dbc);

/* report.c */
void case_add(ProbeCtx* ctx, const char* group, const char* name,
              ProbeStatus status, const char* detail_fmt, ...);
void case_set_status(ProbeCtx* ctx, int index, ProbeStatus status, const char* detail_fmt, ...);
void report_json(ProbeCtx* ctx);
void report_summary(ProbeCtx* ctx);

/* case groups; each returns count of registered cases */
int cases_connect(ProbeCtx* ctx);
int cases_connect_verify(ProbeCtx* ctx, HDBC dbc);
int cases_exec(ProbeCtx* ctx);
int cases_stream(ProbeCtx* ctx);
int cases_cancel(ProbeCtx* ctx);
int cases_tx(ProbeCtx* ctx);

#endif
