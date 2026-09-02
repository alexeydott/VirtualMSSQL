#include "probe.h"
#include <string.h>

void diag_reset(ProbeDiag* d)
{
    memset(d, 0, sizeof(*d));
    strcpy_s(d->sqlstate, sizeof(d->sqlstate), "00000");
}

void diag_capture(ProbeCtx* ctx, SQLSMALLINT htype, SQLHANDLE h, ProbeDiag* out)
{
    SQLWCHAR state[6];
    SQLWCHAR msg[1024];
    SQLINTEGER native = 0;
    SQLSMALLINT len = 0;
    SQLRETURN r;
    (void)ctx;

    diag_reset(out);
    if (!h) return;

    r = SQLGetDiagRecW(htype, h, 1, state, &native, msg, (SQLSMALLINT)(sizeof(msg)/sizeof(msg[0])), &len);
    if (r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO) {
        size_t i;
        WideCharToMultiByte(CP_UTF8, 0, (LPCWCH)state, -1, out->sqlstate, sizeof(out->sqlstate), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, (LPCWCH)msg, -1, out->message, sizeof(out->message), NULL, NULL);
        for (i = 0; out->message[i]; i++) {
            if (out->message[i] == '\r' || out->message[i] == '\n') out->message[i] = ' ';
        }
        out->native = (int)native;
        out->present = true;
    }
}
