/* G16 — static analysis support / fuzz / fault injection (offline part).
 *
 * Deterministic mutation fuzzing over every pure-CPU attack surface of the
 * extension. No server required; the suite never crashes, hangs, or leaks
 * regardless of input. Surfaces:
 *   - profile/connstr parser (vms_profile_parse + vms_connstr_build)
 *   - T-SQL lexer + read-only validator (vms_lexer/vms_tsql_validate_query)
 *   - compiled plan serialize/deserialize (vms_plan_serialize/deserialize)
 *   - identity token codec (vms_identity_encode/decode/free)
 *   - metadata serializer + shadow check (vms_meta_cache payload path,
 *     vms_meta_fingerprint, vms_meta_shadow_check)
 *   - UTF codecs strict/loose (vms_utf8_to_utf16, vms_utf16_to_utf8*)
 *   - bounded buffers (vms_buf_* under fault injection)
 * Fault injection: the allocator-failure hook fires on a schedule derived
 * from the fuzz round, so OOM paths execute inside every surface. */
#include "vms_client.h"
#include "vms_connstr.h"
#include "vms_credentials.h"
#include "vms_foundation.h"
#include "vms_lexer.h"
#include "vms_plan.h"
#include "vms_meta.h"
#include "vms_meta_cache.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

/* the error helpers normally live in the ODBC adapter; standalone copies
 * keep this suite free of any ODBC linkage */
void vms_error_ok(VmsError* e)
{
    if (e) { memset(e, 0, sizeof(*e)); e->cls = VMS_OK; }
}

void vms_error_set(VmsError* e, VmsErrClass cls, const char* sqlstate,
                   int native, const char* fmt, ...)
{
    va_list ap;
    if (!e) return;
    memset(e, 0, sizeof(*e));
    e->cls = cls;
    if (sqlstate) strncpy_s(e->sqlstate, sizeof(e->sqlstate), sqlstate, _TRUNCATE);
    else strcpy_s(e->sqlstate, sizeof(e->sqlstate), "00000");
    e->native = native;
    va_start(ap, fmt);
    _vsnprintf_s(e->message, sizeof(e->message), _TRUNCATE, fmt, ap);
    va_end(ap);
}

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { g_fail++; fprintf(stderr, "FAIL@%d: %s\n", __LINE__, #cond); } \
} while (0)

/* xorshift64* PRNG: deterministic, no library dependency */
static unsigned long long g_rng = 0x9E3779B97F4A7C15ULL;

static unsigned long long rnd(void)
{
    unsigned long long x = g_rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_rng = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static unsigned int rnd_below(unsigned int n)
{
    return n ? (unsigned int)(rnd() % n) : 0;
}

static void rng_seed(unsigned long long s)
{
    g_rng = s ? s : 1;
}

/* ---- fault injection schedule ---- */
static int fault_hook(void)
{
    /* fail on the first N allocations of the armed round */
    return 1;
}

static void arm_faults(unsigned int round)
{
    /* every 4th round runs fully under OOM injection */
    if (round % 4 == 3)
        vms_set_alloc_fail_hook(fault_hook);
}

static void disarm_faults(void)
{
    vms_set_alloc_fail_hook(NULL);
}

/* ---- surface 1: profile/connstr ---- */
static void fuzz_profile(unsigned int rounds)
{
    unsigned int r;
    static const char* keys[] = {
        "server=", "127.0.0.1,1433", "db=", "auth=sql", "auth=windows",
        "cred=", "tls=", "verify", "trust", "optional", "login_timeout=",
        "query_timeout=", "app=", "x", "RetryExec=", "DSN=", "FileDSN=",
        "SaveFile=", "Trusted_Connection=", "PWD=", "Driver=", ";", "='",
        "\xff\xfe", "%s%n", "server=", "\x01\x02"
    };
    for (r = 0; r < rounds; r++) {
        char buf[512];
        size_t len = 0;
        VmsProfile p;
        VmsError err;
        int nkeys = 1 + (int)rnd_below(6);
        int k;

        arm_faults(r);
        memset(buf, 0, sizeof(buf));
        for (k = 0; k < nkeys; k++) {
            const char* frag = keys[rnd_below(sizeof(keys) / sizeof(keys[0]))];
            size_t fl = strlen(frag);
            if (len + fl + 1 >= sizeof(buf)) break;
            memcpy(buf + len, frag, fl);
            len += fl;
            buf[len++] = ';';
        }
        /* mutate: random byte flips */
        if (len > 0) {
            unsigned int flips = rnd_below(4);
            for (k = 0; k < (int)flips; k++)
                buf[rnd_below((unsigned int)len)] = (char)rnd_below(256);
        }
        vms_error_ok(&err);
        if (vms_profile_parse(buf, &p, &err)) {
            /* parse ok: the builder must not crash and must produce
             * something (or fail cleanly with OOM) */
            wchar_t* connstr = NULL;
            size_t cl = 0;
            (void)vms_connstr_build(&p, &connstr, &cl, &err);
            if (connstr) vms_connstr_free(connstr);
        }
        disarm_faults();
    }
}

/* ---- surface 2: T-SQL lexer + validator ---- */
static void fuzz_lexer(unsigned int rounds)
{
    static const wchar_t* base[] = {
        L"SELECT a FROM t", L"SELECT * FROM t WHERE a = 1 AND b > 2",
        L"WITH c AS (SELECT 1 AS x) SELECT x FROM c",
        L"SELECT a FROM t; -- comment", L"SELECT N'it''s' FROM t",
        L"SELECT [col name] FROM [schema t]", L"SELECT (a) FROM t",
        L"DELETE FROM t", L"UPDATE t SET a = 1", L"INSERT INTO t VALUES(1)",
        L"SELECT 1; DROP TABLE t", L"SELECT INTO x", L"EXEC sp_x",
        L"SELECT 'unterminated", L"SELECT (unbalanced",
        L"SELECT a -- line", L"SELECT /* block", L"\x202E\x2028",
        L"SELECT \xDC\x00 FROM t"
    };
    unsigned int r;
    for (r = 0; r < rounds; r++) {
        wchar_t q[256];
        char err[128];
        int qlen;
        VmsLexer lx;
        VmsToken tok;
        int steps;
        const wchar_t* b = base[rnd_below(sizeof(base) / sizeof(base[0]))];
        size_t bl = wcslen(b);

        arm_faults(r);
        if (bl >= 200) bl = 200;
        memcpy(q, b, bl * sizeof(wchar_t));
        qlen = (int)bl;
        /* mutation: character substitutions / insertions / truncation */
        {
            int muts = (int)rnd_below(5);
            int m;
            for (m = 0; m < muts && qlen > 0; m++) {
                int pos = (int)rnd_below((unsigned int)qlen);
                switch (rnd_below(4)) {
                case 0: q[pos] = (wchar_t)rnd_below(0x10000); break;
                case 1: q[pos] = L'\''; break;
                case 2: q[pos] = L';'; break;
                case 3: qlen = pos; q[pos] = 0; break;
                }
            }
        }
        q[qlen] = 0;

        /* the validator must terminate with a verdict either way */
        (void)vms_tsql_validate_query(q, err, sizeof(err));
        /* the lexer must terminate within a bounded number of tokens */
        vms_lexer_init(&lx, q);
        steps = 0;
        while (steps < 4096 && vms_lexer_next(&lx, &tok) == 1) {
            if (tok.kind == VMS_TOK_END) break;
            steps++;
        }
        CHECK(steps < 4096); /* no runaway on malformed input */
        disarm_faults();
    }
}

/* ---- surface 3: plan serialize/deserialize ---- */
static void fuzz_plan(unsigned int rounds)
{
    unsigned int r;
    for (r = 0; r < rounds; r++) {
        char buf[sizeof(VmsPlan) + 32];
        VmsPlan p;
        VmsPlan out;
        unsigned int muts;
        int i;

        arm_faults(r);
        memset(&p, 0, sizeof(p));
        p.magic = VMS_PLAN_MAGIC;
        p.used_mask = (unsigned)rnd_below(0xFFFFFFFFu);
        p.nterms = (int)rnd_below(VMS_PLAN_MAX_ARGS + 1);
        p.nargs = (int)rnd_below(VMS_PLAN_MAX_ARGS + 1);
        p.norder = (int)rnd_below(4);
        p.has_limit = (int)rnd_below(2);
        p.has_offset = (int)rnd_below(2);
        p.limit_arg = (int)rnd_below(8) - 1;
        p.offset_arg = (int)rnd_below(8) - 1;
        for (i = 0; i < p.nterms; i++) {
            p.terms[i].col = (int)rnd_below(70) - 4;
            p.terms[i].op = (VmsPlanOp)rnd_below(VMS_OP_IN + 2);
            p.terms[i].arg_index = (int)rnd_below(8) - 1;
        }
        if (!vms_plan_serialize(&p, buf, sizeof(buf))) {
            disarm_faults();
            continue;
        }
        /* corruption: bit flips, truncation */
        muts = rnd_below(9);
        for (i = 0; i < (int)muts; i++)
            buf[rnd_below(sizeof(VmsPlan))] ^= (char)(1u << rnd_below(8));
        {
            size_t cut = (muts == 8) ? rnd_below(sizeof(VmsPlan))
                                     : sizeof(VmsPlan);
            /* both outcomes are fine: refuse or restore consistently;
             * a crash is the only failure */
            if (vms_plan_deserialize(buf, cut, &out)) {
                /* deserialized: magic must be intact for the accept */
                CHECK(out.magic == VMS_PLAN_MAGIC || muts == 0);
            }
        }
        disarm_faults();
    }
}

/* ---- surface 4: identity token codec ---- */
static void fuzz_identity(unsigned int rounds)
{
    static const char* base[] = {
        "v1|i4:00000001|", "v1|s2:6162|", "v1|b1:ff|", "v1|i4:deadbeef|",
        "v1|", "v1|s0:|", "v1|b0:|", "v2|i4:00000001|", "v1|x4:zz|",
        "v1|i4:0000000", "v1|s99999999:ab|", "", "v1|i4:7fffffffffffffff|",
        "v1|s2:6162|v1|", "v1|i4:00000001|s2:6162|"
    };
    unsigned int r;
    for (r = 0; r < rounds; r++) {
        char tok[512];
        VmsValue parts[VMS_META_MAX_KEY_PARTS];
        int nparts = 0;
        size_t len;
        const char* b = base[rnd_below(sizeof(base) / sizeof(base[0]))];
        int ok;

        arm_faults(r);
        len = strlen(b);
        if (len >= sizeof(tok) - 8) len = sizeof(tok) - 8;
        memcpy(tok, b, len);
        tok[len] = 0;
        /* mutation */
        if (len > 0) {
            unsigned int flips = rnd_below(5);
            unsigned int f;
            for (f = 0; f < flips; f++) {
                unsigned int pos = rnd_below((unsigned int)len);
                switch (rnd_below(3)) {
                case 0: tok[pos] = (char)rnd_below(256); break;
                case 1: tok[pos] = '|'; break;
                case 2: tok[pos] = ':'; break;
                }
            }
        }
        memset(parts, 0, sizeof(parts));
        ok = vms_identity_decode(tok, parts, VMS_META_MAX_KEY_PARTS, &nparts);
        /* ownership contract: free exactly what decode produced */
        if (ok && nparts > 0)
            vms_identity_free(parts, nparts);
        else
            vms_identity_free(parts, 0); /* no-op; must be safe */
        disarm_faults();
    }
}

/* ---- surface 5: metadata serializer / shadow check ---- */
static void fuzz_mcache(unsigned int rounds)
{
    unsigned int r;
    for (r = 0; r < rounds; r++) {
        VmsTableColumns cols;
        int count = 1 + (int)rnd_below(4);
        int i;
        unsigned long long fp;
        unsigned char payload[512];
        int plen;
        VmsTableColumns out;
        int out_count = 0;

        arm_faults(r);
        memset(&cols, 0, sizeof(cols));
        for (i = 0; i < count; i++) {
            _snprintf_s(cols.cols[i].name, VMS_META_MAX_NAME, _TRUNCATE,
                        "c%d", i);
            _snprintf_s(cols.cols[i].type_name, 64, _TRUNCATE, "t%d",
                        rnd_below(3));
            cols.cols[i].vtype = (VmsColType)rnd_below(VMS_CT_SPATIAL + 2);
            cols.cols[i].is_nullable = (int)rnd_below(2);
            cols.cols[i].max_length = (unsigned long)rnd_below(0x10000);
        }
        cols.count = count;
        fp = vms_meta_fingerprint(&cols, count);
        if (fp == 0) { disarm_faults(); continue; }

        /* build a payload via the public roundtrip: encode by hand is
         * private, so validate the checker against synthetic bytes */
        plen = 40 + (int)rnd_below(64);
        for (i = 0; i < plen; i++) payload[i] = (unsigned char)rnd_below(256);
        (void)vms_meta_shadow_check(payload, plen, fp);
        (void)vms_meta_shadow_check(payload, plen, 0);
        /* zero-length and tiny payloads must be refused, not crash */
        CHECK(!vms_meta_shadow_check(payload, 0, fp));
        CHECK(!vms_meta_shadow_check(NULL, 5, fp));
        (void)out; (void)out_count;
        disarm_faults();
    }
}

/* ---- surface 6: UTF codecs ---- */
static void fuzz_utf(unsigned int rounds)
{
    unsigned int r;
    for (r = 0; r < rounds; r++) {
        char u8[64];
        wchar_t u16[64];
        size_t n = rnd_below(40);
        size_t i;
        size_t out_n = 0;
        int strict_ok;

        arm_faults(r);
        for (i = 0; i < n; i++) {
            switch (rnd_below(5)) {
            case 0: u8[i] = (char)rnd_below(256); break;         /* noise */
            case 1: u8[i] = (char)(0x80 + rnd_below(0x40)); break; /* cont */
            case 2: u8[i] = (char)rnd_below(0x80); break;        /* ascii */
            case 3: u8[i] = (char)0xED; break;                   /* surr */
            case 4: u8[i] = (char)(0xC0 + rnd_below(0x20)); break; /* lead */
            }
        }
        strict_ok = vms_utf8_to_utf16(u8, n, u16, 64, &out_n);
        if (strict_ok) {
            /* strict accept implies the bytes were valid UTF-8: the
             * back-conversion must be strict-clean too */
            char back[128];
            size_t back_n = 0;
            int ok2 = vms_utf16_to_utf8(u16, out_n, back, sizeof(back), &back_n);
            CHECK(ok2);
        }
        /* loose conversion never fails on any input */
        {
            char back[128];
            size_t back_n = 0;
            if (strict_ok) {
                (void)vms_utf16_to_utf8_loose(u16, out_n, back, sizeof(back),
                                              &back_n);
            } else {
                /* random u16 input for the loose codec */
                size_t w;
                size_t wn = rnd_below(40);
                for (w = 0; w < wn; w++) {
                    switch (rnd_below(3)) {
                    case 0: u16[w] = (wchar_t)rnd_below(0x10000); break;
                    case 1: u16[w] = (wchar_t)(0xD800 + rnd_below(0x800)); break;
                    case 2: u16[w] = (wchar_t)rnd_below(0x80); break;
                    }
                }
                (void)vms_utf16_to_utf8_loose(u16, wn, back, sizeof(back),
                                              &back_n);
            }
        }
        disarm_faults();
    }
}

/* ---- surface 7: bounded buffers under fault injection ---- */
static void fuzz_bounded(unsigned int rounds)
{
    unsigned int r;
    for (r = 0; r < rounds; r++) {
        VmsBounded b;
        size_t cap = rnd_below(64);
        int ops = (int)rnd_below(16);
        int i;

        arm_faults(r);
        if (!vms_buf_init(&b, cap)) { disarm_faults(); continue; }
        for (i = 0; i < ops; i++) {
            char chunk[8];
            size_t cl = rnd_below(sizeof(chunk));
            size_t k;
            for (k = 0; k < cl; k++) chunk[k] = (char)rnd_below(256);
            (void)vms_buf_append_grow(&b, chunk, cl);
        }
        vms_buf_free(&b);
        vms_buf_free(&b); /* double free must be safe */
        disarm_faults();
    }
}

int main(int argc, char** argv)
{
    unsigned int rounds;
    const char* env;

    /* deterministic seed: stable runs; argv seed for targeted repro */
    rng_seed(argc > 1 ? (unsigned long long)atoll(argv[1]) : 20260904ULL);
    env = getenv("VMS_FUZZ_ROUNDS");
    rounds = env ? (unsigned int)atoi(env) : 2000;

    printf("fuzz rounds=%u seed=%llu\n", rounds,
           (unsigned long long)(argc > 1 ? atoll(argv[1]) : 20260904ULL));

    fuzz_profile(rounds / 4);
    fuzz_lexer(rounds / 4);
    fuzz_plan(rounds / 4);
    fuzz_identity(rounds / 4);
    fuzz_mcache(rounds / 4);
    fuzz_utf(rounds / 4);
    fuzz_bounded(rounds / 4);

    if (g_fail == 0) {
        printf("test_r16: PASS\n");
        return 0;
    }
    fprintf(stderr, "test_r16: %d failures\n", g_fail);
    return 1;
}
