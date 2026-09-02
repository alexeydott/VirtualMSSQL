#include "probe.h"
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>

/* dynamic wide string buffer */
typedef struct WBuf {
    wchar_t* s;
    size_t len;
    size_t cap;
} WBuf;

static void wbuf_init(WBuf* b)
{
    b->cap = 256;
    b->len = 0;
    b->s = (wchar_t*)malloc(b->cap * sizeof(wchar_t));
    if (b->s) b->s[0] = 0;
}

static void wbuf_add(WBuf* b, const wchar_t* add)
{
    size_t alen = wcslen(add);
    if (!b->s) return;
    if (b->len + alen + 1 > b->cap) {
        size_t ncap = b->cap * 2;
        while (b->len + alen + 1 > ncap) ncap *= 2;
        b->s = (wchar_t*)realloc(b->s, ncap * sizeof(wchar_t));
        if (!b->s) return;
        b->cap = ncap;
    }
    memcpy(b->s + b->len, add, (alen + 1) * sizeof(wchar_t));
    b->len += alen;
}

static void wbuf_free(WBuf* b)
{
    free(b->s);
    b->s = NULL;
    b->len = b->cap = 0;
}
#define VMS_UNUSED(x) ((void)(x))

bool env_create(ProbeCtx* ctx)
{
    SQLHENV env = SQL_NULL_HENV;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env))) {
        logf_ctx(ctx, "env: SQLAllocHandle(ENV) failed");
        return false;
    }
    if (!SQL_SUCCEEDED(SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION,
                                     (SQLPOINTER)SQL_OV_ODBC3_80, 0))) {
        logf_ctx(ctx, "env: failed to set ODBC 3.8 version");
        SQLFreeHandle(SQL_HANDLE_ENV, env);
        return false;
    }
    ctx->env = env;
    logf_ctx(ctx, "env: ODBC 3.8 environment allocated");
    return true;
}

void env_destroy(ProbeCtx* ctx)
{
    if (ctx->env != SQL_NULL_HENV) {
        SQLFreeHandle(SQL_HANDLE_ENV, ctx->env);
        ctx->env = SQL_NULL_HENV;
    }
}

bool driver_detect(ProbeCtx* ctx)
{
    SQLWCHAR name[512];
    SQLWCHAR attrs[1024];
    SQLSMALLINT name_len = 0, attrs_len = 0;
    SQLRETURN r;
    bool found = false;

    r = SQLDriversW(ctx->env, SQL_FETCH_FIRST, name, (SQLSMALLINT)(sizeof(name)/sizeof(name[0])),
                    &name_len, attrs, (SQLSMALLINT)(sizeof(attrs)/sizeof(attrs[0])), &attrs_len);
    while (SQL_SUCCEEDED(r)) {
        char name_u8[512];
        WideCharToMultiByte(CP_UTF8, 0, (LPCWCH)name, -1, name_u8, sizeof(name_u8), NULL, NULL);
        if (strstr(name_u8, "ODBC Driver 18 for SQL Server")) {
            found = true;
            logf_ctx(ctx, "driver: found '%s'", name_u8);
        } else {
            logf_ctx(ctx, "driver: available '%s'", name_u8);
        }
        r = SQLDriversW(ctx->env, SQL_FETCH_NEXT, name, (SQLSMALLINT)(sizeof(name)/sizeof(name[0])),
                        &name_len, attrs, (SQLSMALLINT)(sizeof(attrs)/sizeof(attrs[0])), &attrs_len);
    }
    ctx->driver_present = found;
    if (!found) {
        logf_ctx(ctx, "driver: 'ODBC Driver 18 for SQL Server' NOT installed");
    }
    return found;
}

/* server spec: "host", "host\\instance", "host,port", "host\\instance,port" */
static void wbuf_add_server(WBuf* b, const char* server)
{
    char part[512];
    size_t i = 0, n;
    n = strlen(server);
    while (i < n && i < sizeof(part) - 1) {
        char c = server[i];
        if (c == ':' || c == '/') break;
        part[i++] = c;
    }
    part[i] = 0;
    wbuf_add(b, L"Server=");
    {
        wchar_t* w = NULL;
        int wl = MultiByteToWideChar(CP_UTF8, 0, part, -1, NULL, 0);
        if (wl > 0) {
            w = (wchar_t*)malloc((size_t)wl * sizeof(wchar_t));
            MultiByteToWideChar(CP_UTF8, 0, part, -1, w, wl);
            wbuf_add(b, w);
            free(w);
        }
    }
    /* optional port after ':' */
    {
        const char* p = strchr(server, ':');
        if (!p) {
            const char* slash = strchr(server, '/');
            p = slash ? strchr(slash, ':') : NULL;
        }
        if (p) {
            wchar_t port[16];
            long v = atol(p + 1);
            _snwprintf_s(port, 16, _TRUNCATE, L",%ld", v);
            wbuf_add(b, port);
        }
    }
    wbuf_add(b, L";");
}

wchar_t* connstr_build(ProbeCtx* ctx, const char* server, const char* auth_mode,
                       const char* tls_mode, const char* extra)
{
    WBuf b;
    wchar_t tmp[256];
    const ProbeConfig* cfg = &ctx->cfg;

    wbuf_init(&b);
    wbuf_add(&b, L"Driver={ODBC Driver 18 for SQL Server};");

    wbuf_add_server(&b, server);
    wbuf_add(&b, L";");

    if (cfg->database && cfg->database[0]) {
        wbuf_add(&b, L"Database=");
        {
            wchar_t* w = NULL;
            int wl = MultiByteToWideChar(CP_UTF8, 0, cfg->database, -1, NULL, 0);
            if (wl > 0) {
                w = (wchar_t*)malloc((size_t)wl * sizeof(wchar_t));
                MultiByteToWideChar(CP_UTF8, 0, cfg->database, -1, w, wl);
                wbuf_add(&b, w);
                free(w);
            }
        }
        wbuf_add(&b, L";");
    }

    if (strcmp(auth_mode, "windows") == 0) {
        wbuf_add(&b, L"Trusted_Connection=Yes;");
    } else {
        wbuf_add(&b, L"UID=");
        {
            wchar_t* w = NULL;
            int wl = MultiByteToWideChar(CP_UTF8, 0, cfg->sql_user ? cfg->sql_user : "", -1, NULL, 0);
            if (wl > 0) {
                w = (wchar_t*)malloc((size_t)wl * sizeof(wchar_t));
                MultiByteToWideChar(CP_UTF8, 0, cfg->sql_user ? cfg->sql_user : "", -1, w, wl);
                wbuf_add(&b, w);
                free(w);
            }
            wbuf_add(&b, L";PWD=");
            wl = MultiByteToWideChar(CP_UTF8, 0, cfg->sql_password ? cfg->sql_password : "", -1, NULL, 0);
            if (wl > 0) {
                w = (wchar_t*)malloc((size_t)wl * sizeof(wchar_t));
                MultiByteToWideChar(CP_UTF8, 0, cfg->sql_password ? cfg->sql_password : "", -1, w, wl);
                wbuf_add(&b, w);
                free(w);
            }
            wbuf_add(&b, L";");
        }
    }

    /* TLS mapping */
    if (strcmp(tls_mode, "verify") == 0) {
        wbuf_add(&b, L"Encrypt=Yes;TrustServerCertificate=No;");
    } else if (strcmp(tls_mode, "trust_server_certificate") == 0) {
        wbuf_add(&b, L"Encrypt=Yes;TrustServerCertificate=Yes;");
    } else if (strcmp(tls_mode, "optional") == 0) {
        wbuf_add(&b, L"Encrypt=Optional;TrustServerCertificate=No;");
    } else if (strcmp(tls_mode, "strict") == 0) {
        wbuf_add(&b, L"Encrypt=Strict;TrustServerCertificate=No;");
    }

    /* deterministic no-reconnect/retry settings
     * (ConnectRetryInterval=0 is rejected by the driver; valid range is 1-60) */
    wbuf_add(&b, L"ConnectRetryCount=0;ConnectRetryInterval=1;MARS_Connection=No;");

    if (cfg->extra_connstr && cfg->extra_connstr[0]) {
        wchar_t* w = NULL;
        int wl = MultiByteToWideChar(CP_UTF8, 0, cfg->extra_connstr, -1, NULL, 0);
        if (wl > 0) {
            w = (wchar_t*)malloc((size_t)wl * sizeof(wchar_t));
            MultiByteToWideChar(CP_UTF8, 0, cfg->extra_connstr, -1, w, wl);
            wbuf_add(&b, w);
            free(w);
        }
        wbuf_add(&b, L";");
    }

    (void)tmp;
    return b.s ? b.s : _wcsdup(L"");
}

bool conn_connect(ProbeCtx* ctx, HDBC dbc, const wchar_t* wcs, ProbeDiag* diag)
{
    SQLRETURN r;
    if (ctx->cfg.login_timeout > 0) {
        SQLSetConnectAttr(dbc, SQL_ATTR_LOGIN_TIMEOUT,
                          (SQLPOINTER)(SQLINTEGER)ctx->cfg.login_timeout, 0);
    }
    r = SQLDriverConnectW(dbc, NULL, (SQLWCHAR*)wcs, SQL_NTS, NULL, 0, NULL, SQL_DRIVER_NOPROMPT);
    if (r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO) {
        diag_reset(diag);
        return true;
    }
    diag_capture(ctx, SQL_HANDLE_DBC, dbc, diag);
    return false;
}

void conn_close(HDBC dbc)
{
    if (dbc != SQL_NULL_HDBC) {
        SQLDisconnect(dbc);
        SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    }
}
