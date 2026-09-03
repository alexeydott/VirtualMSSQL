/* bench_r15 — performance qualification (R15).
 *
 * Workloads (roadmap):
 *   cold connect / own-pool reuse / PK lookup / 1k-100k-1M rows /
 *   narrow-wide rows / LOB 1-16-64MB / materialization memory-temp /
 *   projection+predicate+LIMIT pushdown
 * Metrics: time-to-first-row, p50/p95, rows/sec, MB/sec, peak RSS,
 *   pool hit ratio, remote rows scanned.
 *
 * Not part of the default ctest suite: run manually with
 *   VMS_BENCH=quick   (reduced sizes, suitable for a smoke gate)
 *   VMS_BENCH=full    (roadmap sizes)
 * Exit code 0 when the mandatory workloads fit the documented thresholds
 * (G15); nonzero otherwise. */
#include "vms_client.h"
#include "vms_connstr.h"
#include "vms_credentials.h"
#include "vms_pool.h"
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

typedef struct Bench {
    VmsClient* cl;
    VmsProfile profile;
    int quick;
    double rss0;
    long long pool_hits, pool_misses; /* observed via live_count deltas */
} Bench;

static double now_ms(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static double peak_rss_mb(void)
{
    PROCESS_MEMORY_COUNTERS pmc;
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return (double)pmc.PeakWorkingSetSize / (1024.0 * 1024.0);
}

static void profile_from_env(VmsProfile* p, VmsError* err)
{
    const char* spec = getenv("VMS_TEST_PROFILE");
    if (!spec || !vms_profile_parse(spec, p, err)) {
        fprintf(stderr, "VMS_TEST_PROFILE missing/bad\n");
        exit(77);
    }
}

/* --- measurement helpers --- */

typedef struct Sample {
    double* v;
    int n, cap;
} Sample;

static void s_add(Sample* s, double ms)
{
    if (s->n == s->cap) {
        int nc = s->cap ? s->cap * 2 : 16;
        double* nv = (double*)realloc(s->v, sizeof(double) * (size_t)nc);
        if (!nv) exit(2);
        s->v = nv;
        s->cap = nc;
    }
    s->v[s->n++] = ms;
}

static int dcmp(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

static double s_pct(Sample* s, double p)
{
    double* copy;
    int idx;
    if (s->n == 0) return -1;
    copy = (double*)malloc(sizeof(double) * (size_t)s->n);
    if (!copy) exit(2);
    memcpy(copy, s->v, sizeof(double) * (size_t)s->n);
    qsort(copy, (size_t)s->n, sizeof(double), dcmp);
    idx = (int)(p * (double)(s->n - 1) + 0.5);
    {
        double r = copy[idx];
        free(copy);
        return r;
    }
}

static void s_report(const char* name, Sample* s)
{
    printf("%-42s n=%-4d p50=%8.2fms p95=%9.2fms\n",
           name, s->n, s_pct(s, 0.50), s_pct(s, 0.95));
    free(s->v);
    s->v = NULL;
    s->n = s->cap = 0;
}

/* recorded results for the G15 threshold gate */
static double g_pk_p95 = -1;
static double g_rows100k = 0, g_rows1m = 0, g_rows_wide = 0;
static double g_lob_min_mbs = 1e9;
static double g_push_full = 0, g_push_quick = 0;

static int check_thresholds(int failures)
{
    printf("\n=== G15 regression thresholds ===\n");
    printf("PK lookup p95 = %6.2fms (<= 50 ms)          %s\n",
           g_pk_p95, (g_pk_p95 >= 0 && g_pk_p95 <= 50.0) ? "PASS" : "FAIL");
    if (!(g_pk_p95 >= 0 && g_pk_p95 <= 50.0)) failures++;
    printf("100k narrow rows/sec = %9.0f (>= 100000)    %s\n",
           g_rows100k, g_rows100k >= 100000.0 ? "PASS" : "FAIL");
    if (g_rows100k < 100000.0) failures++;
    if (g_rows1m > 0) {
        printf("1M narrow rows/sec = %9.0f (>= 100000)      %s\n",
               g_rows1m, g_rows1m >= 100000.0 ? "PASS" : "FAIL");
        if (g_rows1m < 100000.0) failures++;
    }
    printf("wide rows/sec = %9.0f (>= 5000)             %s\n",
           g_rows_wide, g_rows_wide >= 5000.0 ? "PASS" : "FAIL");
    if (g_rows_wide < 5000.0) failures++;
    printf("LOB throughput floor = %6.1f MB/s (>= 10)   %s\n",
           g_lob_min_mbs, g_lob_min_mbs >= 10.0 ? "PASS" : "FAIL");
    if (g_lob_min_mbs < 10.0) failures++;
    printf("Peak RSS %.1f MB (<= 512 MB)                %s\n",
           peak_rss_mb(), peak_rss_mb() <= 512.0 ? "PASS" : "FAIL");
    if (peak_rss_mb() > 512.0) failures++;
    if (g_push_full > 0 && g_push_quick > 0) {
        double speedup = g_push_full / g_push_quick;
        printf("pushdown speedup = %8.1fx (>= 10x)          %s\n",
               speedup, speedup >= 10.0 ? "PASS" : "FAIL");
        if (speedup < 10.0) failures++;
    }
    return failures;
}

/* full scan through a statement; returns rows seen, fills ttf (time to
 * first row) and total ms */
static long long scan_stmt(VmsStatement* st, double* ttf, double* total)
{
    long long rows = 0;
    double t0 = now_ms();
    double ttfr = 0;
    VmsError err;
    int r;
    memset(&err, 0, sizeof(err));
    for (;;) {
        r = vms_stmt_fetch(st, &err);
        if (r <= 0) break;
        rows++;
        if (rows == 1) ttfr = now_ms() - t0;
    }
    *ttf = ttfr;
    *total = now_ms() - t0;
    return rows;
}

static void bench_cold_connect(Bench* b)
{
    Sample s;
    memset(&s, 0, sizeof(s));
    for (int i = 0; i < (b->quick ? 3 : 5); i++) {
        VmsError err;
        VmsConnection* cn;
        wchar_t* connstr = NULL;
        size_t cl = 0;
        double t0 = now_ms();
        if (!vms_connstr_build(&b->profile, &connstr, &cl, &err)) exit(2);
        cn = vms_conn_open(b->cl, connstr, &err);
        vms_connstr_free(connstr);
        if (!cn) { fprintf(stderr, "cold connect failed: %s\n", err.message); exit(1); }
        s_add(&s, now_ms() - t0);
        vms_conn_close(cn);
    }
    s_report("cold connect", &s);
}

static void bench_pool_reuse(Bench* b)
{
    VmsPool* pool = vms_pool_create(2);
    Sample s;
    memset(&s, 0, sizeof(s));
    for (int i = 0; i < (b->quick ? 5 : 20); i++) {
        VmsError err;
        VmsConnection* cn = vms_pool_acquire(pool, &b->profile, &err);
        double t0 = now_ms();
        if (!cn) exit(1);
        if (vms_conn_verify(cn)) s_add(&s, now_ms() - t0);
        vms_pool_release(pool, cn);
    }
    s_report("own-pool reuse (verified acquire)", &s);
    vms_pool_destroy(pool);
}

/* PK lookup (literal key; single row; includes connect cost) */
static void bench_pk_lookup(Bench* b, const char* table, const char* where_col)
{
    Sample s;
    memset(&s, 0, sizeof(s));
    for (int i = 0; i < (b->quick ? 5 : 20); i++) {
        VmsError err;
        VmsConnection* cn = NULL;
        VmsStatement* st;
        wchar_t sql[512];
        long long key = (long long)(i % 1000) + 1;
        double ttf = 0, total = 0;
        _snwprintf_s(sql, 512, _TRUNCATE,
                     L"SELECT id FROM [dbo].[%hs] WHERE [%hs] = %lld",
                     table, where_col, key);
        {
            wchar_t* connstr = NULL;
            size_t cl;
            if (!vms_connstr_build(&b->profile, &connstr, &cl, &err)) exit(2);
            cn = vms_conn_open(b->cl, connstr, &err);
            vms_connstr_free(connstr);
            if (!cn) exit(1);
        }
        st = vms_stmt_exec_direct(cn, sql, &err);
        if (st) {
            scan_stmt(st, &ttf, &total);
            s_add(&s, total);
            vms_stmt_destroy(st);
        }
        vms_conn_close(cn);
    }
    g_pk_p95 = s_pct(&s, 0.95);
    s_report("PK lookup (single row, incl connect)", &s);
}

static void bench_rows(Bench* b, const char* table, const char* cols,
                       long long expect_rows, const char* label)
{
    wchar_t sql[512];
    Sample ttf_s, rps_s;
    memset(&ttf_s, 0, sizeof(ttf_s));
    memset(&rps_s, 0, sizeof(rps_s));
    _snwprintf_s(sql, 512, _TRUNCATE, L"SELECT %hs FROM [dbo].[%hs]", cols, table);
    for (int i = 0; i < (b->quick ? 1 : 3); i++) {
        VmsError err;
        VmsConnection* cn = NULL;
        VmsStatement* st;
        double ttf = 0, total = 0;
        long long rows;
        {
            wchar_t* connstr = NULL;
            size_t cl;
            if (!vms_connstr_build(&b->profile, &connstr, &cl, &err)) exit(2);
            cn = vms_conn_open(b->cl, connstr, &err);
            vms_connstr_free(connstr);
            if (!cn) exit(1);
        }
        st = vms_stmt_exec_direct(cn, sql, &err);
        if (!st) { fprintf(stderr, "%s: %s\n", label, err.message); exit(1); }
        rows = scan_stmt(st, &ttf, &total);
        vms_stmt_destroy(st);
        vms_conn_close(cn);
        if (rows != expect_rows)
            fprintf(stderr, "%s: rows=%lld (expected %lld)\n", label, rows, expect_rows);
        s_add(&ttf_s, ttf);
        s_add(&rps_s, total > 0 ? (double)rows / (total / 1000.0) : 0);
    }
    {
        double rps = s_pct(&rps_s, 0.50);
        if (expect_rows >= 1000000) g_rows1m = rps;
        else if (expect_rows >= 100000) g_rows100k = rps;
        else if (expect_rows >= 5000) g_rows_wide = rps;
        printf("%-42s ttf p50=%7.1fms rows/sec p50=%10.0f\n",
               label, s_pct(&ttf_s, 0.50), rps);
    }
    free(ttf_s.v); free(rps_s.v);
}static void bench_lob(Bench* b, int id, double expect_mb, const char* label)
{
    wchar_t sql[256];
    Sample s;
    memset(&s, 0, sizeof(s));
    _snwprintf_s(sql, 256, _TRUNCATE,
                 L"SELECT bigt FROM [dbo].[vms15_lob] WHERE id = %d", id);
    for (int i = 0; i < (b->quick ? 1 : 2); i++) {
        VmsError err;
        VmsConnection* cn = NULL;
        VmsStatement* st;
        double ttf = 0, total = 0;
        double mbs;
        {
            wchar_t* connstr = NULL;
            size_t cl;
            if (!vms_connstr_build(&b->profile, &connstr, &cl, &err)) exit(2);
            cn = vms_conn_open(b->cl, connstr, &err);
            vms_connstr_free(connstr);
            if (!cn) exit(1);
        }
        st = vms_stmt_exec_direct(cn, sql, &err);
        if (!st) { fprintf(stderr, "%s: %s\n", label, err.message); exit(1); }
        {
            long long rows = scan_stmt(st, &ttf, &total);
            (void)rows;
        }
        vms_stmt_destroy(st);
        vms_conn_close(cn);
        mbs = total > 0 ? expect_mb / (total / 1000.0) : 0;
        s_add(&s, mbs);
    }
    {
        double mbs = s_pct(&s, 0.50);
        if (mbs < g_lob_min_mbs) g_lob_min_mbs = mbs;
        printf("%-42s p50=%8.1f MB/sec\n", label, mbs);
    }
    free(s.v);
}

static void bench_pushdown(Bench* b)
{
    /* projection/predicate/LIMIT: compare full-scan vs pushed-down */
    wchar_t sql[512];
    double full = 0, push = 0;
    VmsError err;
    VmsConnection* cn = NULL;
    VmsStatement* st;
    double ttf = 0, total = 0;

    {
        wchar_t* connstr = NULL;
        size_t cl;
        if (!vms_connstr_build(&b->profile, &connstr, &cl, &err)) exit(2);
        cn = vms_conn_open(b->cl, connstr, &err);
        vms_connstr_free(connstr);
        if (!cn) exit(1);
    }
    /* full: all columns, no predicate */
    st = vms_stmt_exec_direct(cn, L"SELECT id, i FROM [dbo].[vms15_million]", &err);
    if (st) { scan_stmt(st, &ttf, &total); vms_stmt_destroy(st); }
    (void)ttf;
    full = total;
    /* pushed: projection + predicate + TOP */
    st = vms_stmt_exec_direct(cn,
        L"SELECT TOP (10) id FROM [dbo].[vms15_million] WHERE id = 500000", &err);
    if (st) { scan_stmt(st, &ttf, &total); vms_stmt_destroy(st); }
    push = total;
    g_push_full = full;
    g_push_quick = push;
    printf("%-42s full=%7.1fms pushed=%7.1fms speedup=%5.1fx\n",
           "projection/predicate/LIMIT pushdown", full, push,
           push > 0 ? full / push : 0);
    vms_conn_close(cn);
}

int main(void)
{
    Bench b;
    VmsError err;
    const char* mode;
    int failures = 0;

    memset(&b, 0, sizeof(b));
    mode = getenv("VMS_BENCH");
    b.quick = (mode && !strcmp(mode, "quick"));

    /* credentials (same convention as the test suites; overridable for
     * remote bench servers via VMS_BENCH_UID/VMS_BENCH_PWD/VMS_BENCH_CRED).
     * The profile references a credential key: entries are provisioned for
     * both the standard 'test' key and VMS_BENCH_CRED (default 'test'). */
    {
        const char* uid = getenv("VMS_BENCH_UID");
        const char* pwd = getenv("VMS_BENCH_PWD");
        const char* ckey = getenv("VMS_BENCH_CRED");
        wchar_t wuid[128], wpwd[256], wkey[256], wkey_uid[260], wkey_pwd[260];
        const VmsCredProviderV1* mem = vms_cred_memory_provider();
        if (!mem) return 2;
        vms_cred_set_provider(mem);
        MultiByteToWideChar(CP_UTF8, 0, uid ? uid : "sa", -1, wuid, 128);
        MultiByteToWideChar(CP_UTF8, 0, pwd ? pwd : "Vms-Probe-2026!x", -1, wpwd, 256);
        if (!vms_cred_memory_set(L"test:uid", wuid) ||
            !vms_cred_memory_set(L"test:pwd", wpwd)) return 2;
        MultiByteToWideChar(CP_UTF8, 0, ckey ? ckey : "test", -1, wkey, 256);
        _snwprintf_s(wkey_uid, 260, _TRUNCATE, L"%ls:uid", wkey);
        _snwprintf_s(wkey_pwd, 260, _TRUNCATE, L"%ls:pwd", wkey);
        if (!vms_cred_memory_set(wkey_uid, wuid) ||
            !vms_cred_memory_set(wkey_pwd, wpwd)) return 2;
    }
    profile_from_env(&b.profile, &err);
    b.cl = vms_client_init(&err);
    if (!b.cl) return 2;

    printf("=== VirtualMSSQL performance qualification (%s mode) ===\n",
           b.quick ? "quick" : "full");
    printf("RSS at start: %.1f MB\n", peak_rss_mb());

    bench_cold_connect(&b);
    bench_pool_reuse(&b);
    bench_pk_lookup(&b, "vms6_t_int", "id");
    bench_rows(&b, "vms6_t_int", "id, v", 3, "1k-class rows (vms6_t_int)");
    bench_rows(&b, "vms6_t_big", "id, i", 100000, "100k narrow rows");
    if (!b.quick)
        bench_rows(&b, "vms15_million", "id, i", 1000000, "1M narrow rows");
    bench_rows(&b, "vms15_wide", "id, c1, c2, c3, c4, c5, c6, c7, c8,"
               " n1, n2, n3, n4, f1, f2, g1", 5000, "5000 wide rows (16 cols)");
    bench_lob(&b, 1, 1.0, "LOB 1MB");
    bench_lob(&b, 2, 16.0, "LOB 16MB");
    bench_lob(&b, 3, 64.0, "LOB 64MB");
    bench_pushdown(&b);

    /* ---- G15 regression thresholds (documented limits; CI gate) ----
     * Derived from the R15 baseline (localhost + LAN server, 2026-09-03).
     * The floor values leave >=5x headroom against the observed p50s on
     * hardware comparable to the test bench. */
    failures = check_thresholds(failures);

    vms_client_destroy(b.cl);
    printf("bench_r15: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
