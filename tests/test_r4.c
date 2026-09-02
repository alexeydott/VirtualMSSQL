/* G4 — config/auth/TLS/identity/pool/redaction tests.
 * Credential ABI + providers, strict profile grammar, connstr builder
 * posture, and pool clean-state reuse against a live server. */
#include "vms_credentials.h"
#include "vms_connstr.h"
#include "vms_pool.h"
#include "vms_client.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { g_fail++; fprintf(stderr, "FAIL@%d: %s\n", __LINE__, #cond); } \
} while (0)

static void test_provider_abi(void)
{
    VmsCredProviderV1 bad;
    memset(&bad, 0, sizeof(bad));
    bad.abi_version = 99; /* future/unknown ABI */
    CHECK(!vms_cred_provider_valid(&bad));
    bad.abi_version = VMS_CRED_PROVIDER_ABI_VERSION;
    bad.name = NULL;
    CHECK(!vms_cred_provider_valid(&bad));
    bad.name = "x";
    bad.secret_begin = NULL;
    CHECK(!vms_cred_provider_valid(&bad));
    CHECK(vms_cred_provider_valid(vms_cred_memory_provider()));
    CHECK(vms_cred_provider_valid(vms_cred_wincred_provider()));
}

static void test_memory_provider(void)
{
    VmsCredentialRef ref;
    wchar_t* secret = NULL;
    size_t n = 0;
    VmsError err;

    CHECK(vms_cred_memory_set(L"db1:pwd", L"secret1"));
    CHECK(vms_cred_memory_set(L"db1:uid", L"sa"));
    CHECK(vms_cred_memory_set(L"db1:pwd", L"secret2")); /* overwrite */

    vms_cred_set_provider(vms_cred_memory_provider());

    wcscpy_s(ref.key, 256, L"db1:pwd");
    CHECK(vms_cred_secret_begin(&ref, &secret, &n, &err));
    if (secret) {
        CHECK(n == 7 && wcscmp(secret, L"secret2") == 0);
        vms_cred_secret_end(&ref, secret, n);
    }
    /* unknown key */
    wcscpy_s(ref.key, 256, L"nope");
    CHECK(!vms_cred_secret_begin(&ref, &secret, &n, &err));
    CHECK(err.cls == VMS_ERR_INVALID_ARG);
}

static void test_profile_grammar(void)
{
    VmsProfile p;
    VmsError err;

    /* valid */
    CHECK(vms_profile_parse("server=localhost,1433;db=testdb;auth=sql;cred=c1;"
                            "tls=optional;login_timeout=5;query_timeout=30;app=MyApp",
                            &p, &err));
    CHECK(wcscmp(p.server, L"localhost,1433") == 0);
    CHECK(wcscmp(p.database, L"testdb") == 0);
    CHECK(p.auth == VMS_AUTH_SQL);
    CHECK(p.tls == VMS_TLS_OPTIONAL);
    CHECK(p.login_timeout_sec == 5);
    CHECK(p.query_timeout_sec == 30);
    CHECK(wcscmp(p.app_name, L"MyApp") == 0);

    /* defaults: tls=verify (TZ mandatory default); windows auth needs no cred */
    CHECK(vms_profile_parse("server=h1;auth=windows", &p, &err));
    CHECK(p.tls == VMS_TLS_VERIFY);
    CHECK(p.auth == VMS_AUTH_WINDOWS);
    /* auth=sql without cred -> rejected */
    CHECK(!vms_profile_parse("server=h1;auth=sql", &p, &err));
    CHECK(err.cls == VMS_ERR_INVALID_ARG);
    /* auth=windows without cred -> ok */
    CHECK(vms_profile_parse("server=h1;auth=windows", &p, &err));
    CHECK(p.auth == VMS_AUTH_WINDOWS);
    /* bad tls value */
    CHECK(!vms_profile_parse("server=h1;tls=off", &p, &err));
    /* unknown key (injection attempt) */
    CHECK(!vms_profile_parse("server=h1;RetryExec=1", &p, &err));
    CHECK(!vms_profile_parse("server=h1;DSN=whatever", &p, &err));
    CHECK(!vms_profile_parse("server=h1;FileDSN=x", &p, &err));
    CHECK(!vms_profile_parse("server=h1;Trusted_Connection=Yes", &p, &err));
    /* no server */
    CHECK(!vms_profile_parse("db=x", &p, &err));
    /* malformed token */
    CHECK(!vms_profile_parse("server", &p, &err));
}

static void test_connstr_posture(void)
{
    VmsProfile p;
    VmsError err;
    wchar_t* s = NULL;
    size_t n = 0;

    vms_cred_set_provider(vms_cred_memory_provider());
    vms_cred_memory_set(L"c1:uid", L"sa");
    vms_cred_memory_set(L"c1:pwd", L"TopSecret1!");

    /* verify TLS posture */
    CHECK(vms_profile_parse("server=localhost,1433;auth=sql;cred=c1", &p, &err));
    CHECK(vms_connstr_build(&p, &s, &n, &err));
    if (s) {
        CHECK(wcsstr(s, L"Encrypt=Yes;TrustServerCertificate=No;") != NULL);
        CHECK(wcsstr(s, L"ConnectRetryCount=0;") != NULL);
        CHECK(wcsstr(s, L"MARS_Connection=No;") != NULL);
        CHECK(wcsstr(s, L"UID=sa;") != NULL);
        CHECK(wcsstr(s, L"PWD=TopSecret1!") != NULL);
        /* forbidden keys must never appear */
        CHECK(wcsstr(s, L"RetryExec") == NULL);
        CHECK(wcsstr(s, L"DSN=") == NULL);
        CHECK(wcsstr(s, L"SaveFile") == NULL);
        vms_connstr_free(s);
        s = NULL;
    }

    /* trust posture */
    CHECK(vms_profile_parse("server=h;auth=sql;cred=c1;tls=trust", &p, &err));
    CHECK(vms_connstr_build(&p, &s, &n, &err));
    if (s) {
        CHECK(wcsstr(s, L"Encrypt=Yes;TrustServerCertificate=Yes;") != NULL);
        vms_connstr_free(s);
        s = NULL;
    }

    /* windows auth posture: no UID/PWD at all */
    CHECK(vms_profile_parse("server=h;auth=windows", &p, &err));
    CHECK(vms_connstr_build(&p, &s, &n, &err));
    if (s) {
        CHECK(wcsstr(s, L"Trusted_Connection=Yes;") != NULL);
        CHECK(wcsstr(s, L"UID=") == NULL);
        CHECK(wcsstr(s, L"PWD=") == NULL);
        vms_connstr_free(s);
        s = NULL;
    }

    /* missing provider entries -> clean failure */
    CHECK(vms_profile_parse("server=h;auth=sql;cred=ghost", &p, &err));
    CHECK(!vms_connstr_build(&p, &s, &n, &err));
    CHECK(err.cls == VMS_ERR_INVALID_ARG);
    CHECK(s == NULL);
}

/* ---- live-server pool tests ---- */

static int load_profile(VmsProfile* p, VmsError* err)
{
    const char* env = getenv("VMS_TEST_PROFILE");
    if (!env || !env[0]) return 0;
    return vms_profile_parse(env, p, err);
}

static void test_pool(void)
{
    VmsProfile p;
    VmsError err;
    VmsPool* pool;
    VmsConnection* a;
    VmsConnection* b;
    VmsStatement* st;

    vms_cred_set_provider(vms_cred_memory_provider());
    vms_cred_memory_set(L"test:uid", L"sa");
    vms_cred_memory_set(L"test:pwd", L"Vms-Probe-2026!x");

    {
        const char* env = getenv("VMS_TEST_PROFILE");
        if (!env || !env[0]) return;
        if (!vms_profile_parse(env, &p, &err)) return;
    }

    pool = vms_pool_create(2);
    CHECK(pool != NULL);
    if (!pool) return;

    CHECK(vms_pool_idle_count(pool) == 0);
    CHECK(vms_pool_live_count(pool) == 0);

    a = vms_pool_acquire(pool, &p, &err);
    CHECK(a != NULL);
    if (!a) { vms_pool_destroy(pool); return; }
    CHECK(vms_pool_live_count(pool) == 1);

    /* release -> idle (clean state) */
    vms_pool_release(pool, a);
    CHECK(vms_pool_idle_count(pool) == 1);
    CHECK(vms_pool_live_count(pool) == 1);

    /* acquire again -> reuse (no new connection) */
    a = vms_pool_acquire(pool, &p, &err);
    CHECK(a != NULL);
    CHECK(vms_pool_idle_count(pool) == 0);

    /* second concurrent connection */
    b = vms_pool_acquire(pool, &p, &err);
    CHECK(b != NULL);
    CHECK(a != b);
    CHECK(vms_pool_live_count(pool) == 2);

    /* dirty release: open transaction -> must NOT return to the pool */
    CHECK(vms_tran_begin(a, &err) == 0);
    st = vms_stmt_exec_direct(a, L"SELECT 1", &err);
    if (st) vms_stmt_destroy(st);
    vms_pool_release(pool, a);
    CHECK(vms_pool_idle_count(pool) == 0);
    CHECK(vms_pool_live_count(pool) == 1);

    vms_pool_release(pool, b);
    CHECK(vms_pool_idle_count(pool) == 1);

    vms_pool_destroy(pool);
}

int main(void)
{
    VmsProfile p;
    VmsError err;

    test_provider_abi();
    test_memory_provider();
    test_profile_grammar();
    test_connstr_posture();

    /* live pool test only when VMS_TEST_PROFILE is provided */
    if (load_profile(&p, &err)) {
        test_pool();
    } else {
        fprintf(stderr, "VMS_TEST_PROFILE not set; live pool tests skipped\n");
    }

    if (g_fail == 0) {
        printf("test_r4: PASS\n");
        return 0;
    }
    fprintf(stderr, "test_r4: %d failures\n", g_fail);
    return 1;
}
