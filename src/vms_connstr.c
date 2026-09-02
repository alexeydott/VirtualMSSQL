/* vms_connstr.c — profile parsing and strict connection-string builder (R4). */
#include "vms_connstr.h"
#include "vms_foundation.h"
#include "vms_log.h"
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

void vms_profile_defaults(VmsProfile* p)
{
    memset(p, 0, sizeof(*p));
    p->auth = VMS_AUTH_SQL;
    p->tls = VMS_TLS_VERIFY; /* TZ mandatory default */
    p->login_timeout_sec = 10;
    p->query_timeout_sec = 0;
    wcscpy_s(p->app_name, sizeof(p->app_name) / sizeof(p->app_name[0]), L"VirtualMSSQL");
}

/* --- strict-grammar parser (UTF-8 input) --- */

typedef struct KV {
    char key[32];
    char val[256];
} KV;

static int split_kv(char* s, KV* kv)
{
    char* eq = strchr(s, '=');
    size_t klen, vlen;
    if (!eq) return 0;
    *eq = 0;
    klen = strlen(s);
    vlen = strlen(eq + 1);
    if (klen == 0 || klen >= sizeof(kv->key) || vlen >= sizeof(kv->val)) return 0;
    memcpy(kv->key, s, klen + 1);
    memcpy(kv->val, eq + 1, vlen + 1);
    /* trim spaces */
    {
        char* k = kv->key;
        while (*k == ' ') k++;
        memmove(kv->key, k, strlen(k) + 1);
    }
    return 1;
}

static int eqi(const char* a, const char* b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int copy_wide(const char* src, wchar_t* dst, size_t dst_chars)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, NULL, 0);
    if (n <= 0 || (size_t)n > dst_chars) return 0;
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, dst, n);
    return 1;
}

int vms_profile_parse(const char* spec, VmsProfile* p, VmsError* err)
{
    char* buf;
    char* ctxs = NULL;
    char* tok;
    int have_server = 0;

    vms_error_ok(err);
    if (!spec || !p) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "profile parse: bad args");
        return 0;
    }
    vms_profile_defaults(p);

    buf = _strdup(spec);
    if (!buf) {
        vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM profile parse");
        return 0;
    }
    tok = strtok_s(buf, ";", &ctxs);
    while (tok) {
        KV kv;
        if (!split_kv(tok, &kv)) {
            vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0,
                          "invalid token '%s' (expected key=value)", tok);
            free(buf);
            return 0;
        }
        if (eqi(kv.key, "server") || eqi(kv.key, "host")) {
            if (!copy_wide(kv.val, p->server, 256)) {
                vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "server too long/invalid");
                free(buf);
                return 0;
            }
            have_server = 1;
        } else if (eqi(kv.key, "db") || eqi(kv.key, "database")) {
            if (!copy_wide(kv.val, p->database, 128)) {
                vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "db too long/invalid");
                free(buf);
                return 0;
            }
        } else if (eqi(kv.key, "auth")) {
            if (eqi(kv.val, "sql")) p->auth = VMS_AUTH_SQL;
            else if (eqi(kv.val, "windows")) p->auth = VMS_AUTH_WINDOWS;
            else {
                vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0,
                              "auth must be sql|windows, got '%s'", kv.val);
                free(buf);
                return 0;
            }
        } else if (eqi(kv.key, "cred")) {
            if (!copy_wide(kv.val, p->cred.key, 256)) {
                vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "cred key too long");
                free(buf);
                return 0;
            }
        } else if (eqi(kv.key, "tls")) {
            if (eqi(kv.val, "verify")) p->tls = VMS_TLS_VERIFY;
            else if (eqi(kv.val, "trust_server_certificate") || eqi(kv.val, "trust")) p->tls = VMS_TLS_TRUST_SERVER_CERTIFICATE;
            else if (eqi(kv.val, "optional")) p->tls = VMS_TLS_OPTIONAL;
            else {
                vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0,
                              "tls must be verify|trust_server_certificate|optional, got '%s'", kv.val);
                free(buf);
                return 0;
            }
        } else if (eqi(kv.key, "login_timeout")) {
            p->login_timeout_sec = atoi(kv.val);
            if (p->login_timeout_sec < 0) p->login_timeout_sec = 0;
        } else if (eqi(kv.key, "query_timeout")) {
            p->query_timeout_sec = atoi(kv.val);
            if (p->query_timeout_sec < 0) p->query_timeout_sec = 0;
        } else if (eqi(kv.key, "app")) {
            if (!copy_wide(kv.val, p->app_name, 64)) {
                vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "app name too long");
                free(buf);
                return 0;
            }
        } else {
            vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0,
                          "unknown key '%s' (strict grammar)", kv.key);
            free(buf);
            return 0;
        }
        tok = strtok_s(NULL, ";", &ctxs);
    }
    free(buf);

    if (!have_server || !p->server[0]) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "server is required");
        return 0;
    }
    if (p->auth == VMS_AUTH_SQL && !p->cred.key[0]) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0,
                      "auth=sql requires cred=<credential key>");
        return 0;
    }
    return 1;
}

/* --- strict builder --- */

typedef struct WAcc {
    wchar_t* s;
    size_t cap;
    size_t len;
} WAcc;

static int wacc_reserve(WAcc* a, size_t extra)
{
    size_t need;
    if (!vms_add_sz(a->len, extra, &need) || need + 1 > a->cap) {
        size_t ncap = a->cap ? a->cap * 2 : 512;
        wchar_t* ns;
        while (a->len + extra + 1 > ncap) ncap *= 2;
        ns = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, ncap * sizeof(wchar_t));
        if (!ns) return 0;
        if (a->s) {
            memcpy(ns, a->s, a->len * sizeof(wchar_t));
            HeapFree(GetProcessHeap(), 0, a->s);
        } else {
            ns[0] = 0;
        }
        a->s = ns;
        a->cap = ncap;
    }
    return 1;
}

static int wacc_add(WAcc* a, const wchar_t* s)
{
    size_t n = wcslen(s);
    if (!wacc_reserve(a, n)) return 0;
    memcpy(a->s + a->len, s, n * sizeof(wchar_t));
    a->len += n;
    a->s[a->len] = 0;
    return 1;
}

static int wacc_add_int(WAcc* a, long v)
{
    wchar_t tmp[24];
    _snwprintf_s(tmp, 24, _TRUNCATE, L"%ld", v);
    return wacc_add(a, tmp);
}

void vms_connstr_free(wchar_t* s)
{
    if (s) {
        HeapFree(GetProcessHeap(), 0, s);
    }
}

int vms_connstr_build(const VmsProfile* p, wchar_t** out, size_t* out_len,
                      VmsError* err)
{
    WAcc a;
    int ok = 1;

    vms_error_ok(err);
    if (!p || !out || !p->server[0]) {
        vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0, "connstr build: bad args");
        return 0;
    }
    memset(&a, 0, sizeof(a));

    ok = ok && wacc_add(&a, L"Driver={ODBC Driver 18 for SQL Server};Server=");
    ok = ok && wacc_add(&a, p->server);
    ok = ok && wacc_add(&a, L";");
    if (p->database[0])
        ok = ok && wacc_add(&a, L"Database=") && wacc_add(&a, p->database) && wacc_add(&a, L";");

    if (p->auth == VMS_AUTH_WINDOWS) {
        ok = ok && wacc_add(&a, L"Trusted_Connection=Yes;");
    } else {
        /* SQL auth: resolve the login name and password through the active
         * credential provider under "<cred>:uid" and "<cred>:pwd". Secrets
         * never outlive this scope and never enter the returned string. */
        wchar_t uid_key[280];
        wchar_t pwd_key[280];
        VmsCredentialRef ru, rp;
        wchar_t* su = NULL;
        wchar_t* sp = NULL;
        size_t lu = 0, lp = 0;
        _snwprintf_s(uid_key, 280, _TRUNCATE, L"%s:uid", p->cred.key);
        _snwprintf_s(pwd_key, 280, _TRUNCATE, L"%s:pwd", p->cred.key);
        wcscpy_s(ru.key, 256, uid_key);
        wcscpy_s(rp.key, 256, pwd_key);
        ok = ok && wacc_add(&a, L"UID=");
        if (vms_cred_secret_begin(&ru, &su, &lu, err) &&
            wacc_add(&a, su) &&
            vms_cred_secret_begin(&rp, &sp, &lp, err)) {
            ok = ok && wacc_add(&a, L";PWD=");
            ok = ok && wacc_add(&a, sp);
            ok = ok && wacc_add(&a, L";");
            vms_cred_secret_end(&rp, sp, lp);
            vms_cred_secret_end(&ru, su, lu);
        } else {
            /* one of the secrets missing */
            if (su) vms_cred_secret_end(&ru, su, lu);
            vms_error_set(err, VMS_ERR_INVALID_ARG, NULL, 0,
                          "credential '%ls' must expose :uid and :pwd entries",
                          p->cred.key);
            if (a.s) HeapFree(GetProcessHeap(), 0, a.s);
            return 0;
        }
    }

    /* TLS mapping */
    switch (p->tls) {
    case VMS_TLS_VERIFY:
        ok = ok && wacc_add(&a, L"Encrypt=Yes;TrustServerCertificate=No;");
        break;
    case VMS_TLS_TRUST_SERVER_CERTIFICATE:
        ok = ok && wacc_add(&a, L"Encrypt=Yes;TrustServerCertificate=Yes;");
        break;
    case VMS_TLS_OPTIONAL:
        ok = ok && wacc_add(&a, L"Encrypt=Optional;TrustServerCertificate=No;");
        break;
    }

    /* deterministic no-retry posture (TZ mandatory) */
    ok = ok && wacc_add(&a, L"ConnectRetryCount=0;ConnectRetryInterval=1;MARS_Connection=No;");

    /* app name */
    ok = ok && wacc_add(&a, L"APP=") && wacc_add(&a, p->app_name) && wacc_add(&a, L";");

    if (p->login_timeout_sec > 0) {
        ok = ok && wacc_add(&a, L"LoginTimeout=");
        ok = ok && wacc_add_int(&a, p->login_timeout_sec);
        ok = ok && wacc_add(&a, L";");
    }

    if (!ok) {
        if (a.s) HeapFree(GetProcessHeap(), 0, a.s);
        vms_error_set(err, VMS_ERR_NO_MEMORY, NULL, 0, "OOM connstr build");
        return 0;
    }
    *out = a.s;
    if (out_len) *out_len = a.len;
    return 1;
}
