/* vms_lexer.c — bounded T-SQL lexer and query-source validator (R8).
 * The lexer is position-based and allocation-free; the validator walks
 * tokens and enforces the read-only SELECT/WITH contract. */
#include "vms_lexer.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

void vms_lexer_init(VmsLexer* lx, const wchar_t* src)
{
    lx->src = src;
    lx->len = src ? wcslen(src) : 0;
    lx->pos = 0;
    lx->depth = 0;
}

static int is_word_start(wchar_t c)
{
    return iswalpha(c) || c == '_' || c == '#' || c == '@';
}

static int is_word_char(wchar_t c)
{
    return iswalnum(c) || c == '_' || c == '#' || c == '@' || c == '$';
}

int vms_lexer_next(VmsLexer* lx, VmsToken* tok)
{
    const wchar_t* s = lx->src;
    size_t i = lx->pos;
    size_t start;

    memset(tok, 0, sizeof(*tok));

    /* skip whitespace and comments */
    for (;;) {
        while (i < lx->len && iswspace(s[i])) i++;
        if (i + 1 < lx->len && s[i] == L'-' && s[i + 1] == L'-') {
            i += 2;
            while (i < lx->len && s[i] != L'\n') i++;
            continue;
        }
        if (i + 1 < lx->len && s[i] == L'/' && s[i + 1] == L'*') {
            i += 2;
            while (i + 1 < lx->len && !(s[i] == L'*' && s[i + 1] == L'/')) i++;
            if (i + 1 >= lx->len) return 0; /* unterminated comment */
            i += 2;
            continue;
        }
        break;
    }

    lx->pos = i;
    if (i >= lx->len) {
        tok->kind = VMS_TOK_END;
        tok->start = i;
        tok->len = 0;
        return 1;
    }

    start = i;

    /* bracketed identifier [name] — treat as word */
    if (s[i] == L'[') {
        i++;
        while (i < lx->len && s[i] != L']') i++;
        if (i >= lx->len) return 0; /* unterminated */
        i++; /* skip ] */
        tok->kind = VMS_TOK_WORD;
        tok->start = start;
        tok->len = i - start;
        {
            /* copy inner text lowercased, limited */
            size_t j, n = 0;
            for (j = start + 1; j < i - 1 && n < 63; j++, n++)
                tok->text[n] = towlower(s[j]);
            tok->text[n] = 0;
        }
        lx->pos = i;
        return 1;
    }

    /* strings: N'...' or '...'; '' escape; do not copy the value */
    if (s[i] == L'\'' || (s[i] == L'N' && i + 1 < lx->len && s[i + 1] == L'\'')) {
        if (s[i] == L'N') i++;
        i++; /* opening quote */
        for (;;) {
            if (i >= lx->len) return 0; /* unterminated string */
            if (s[i] == L'\'') {
                if (i + 1 < lx->len && s[i + 1] == L'\'') { i += 2; continue; }
                i++;
                break;
            }
            i++;
        }
        tok->kind = VMS_TOK_STRING;
        tok->start = start;
        tok->len = i - start;
        lx->pos = i;
        return 1;
    }

    /* numbers */
    if (iswdigit(s[i])) {
        while (i < lx->len && (iswalnum(s[i]) || s[i] == L'.')) i++;
        tok->kind = VMS_TOK_NUMBER;
        tok->start = start;
        tok->len = i - start;
        lx->pos = i;
        return 1;
    }

    /* words */
    if (is_word_start(s[i])) {
        while (i < lx->len && is_word_char(s[i])) i++;
        tok->kind = VMS_TOK_WORD;
        tok->start = start;
        tok->len = i - start;
        {
            size_t j, n = 0;
            for (j = start; j < i && n < 63; j++, n++)
                tok->text[n] = towlower(s[j]);
            tok->text[n] = 0;
        }
        lx->pos = i;
        return 1;
    }

    /* two-char operators */
    if (i + 1 < lx->len) {
        wchar_t c1 = s[i], c2 = s[i + 1];
        if ((c1 == L'<' && (c2 == L'=' || c2 == L'>')) ||
            (c1 == L'>' && c2 == L'=') ||
            (c1 == L'!' && c2 == L'=')) {
            tok->kind = VMS_TOK_OP2;
            tok->start = start;
            tok->len = 2;
            tok->text[0] = c1;
            tok->text[1] = c2;
            tok->text[2] = 0;
            lx->pos = i + 2;
            return 1;
        }
    }

    /* single-char punctuation */
    if (wcschr(L"(),;.*+-/%=<>", s[i])) {
        if (s[i] == L'(') lx->depth++;
        if (s[i] == L')') {
            if (lx->depth == 0) return 0; /* unbalanced */
            lx->depth--;
        }
        tok->kind = VMS_TOK_PUNCT;
        tok->start = start;
        tok->len = 1;
        tok->text[0] = s[i];
        tok->text[1] = 0;
        lx->pos = i + 1;
        return 1;
    }

    return 0; /* unexpected character */
}

/* ---- validator ---- */

static int word_is(const VmsToken* t, const char* kw)
{
    size_t n = strlen(kw);
    size_t i;
    if (t->kind != VMS_TOK_WORD) return 0;
    for (i = 0; i < n; i++) {
        if ((wchar_t)towlower((wchar_t)kw[i]) != t->text[i]) return 0;
    }
    return t->text[n] == 0;
}

static const char* k_forbidden[] = {
    "insert", "update", "delete", "merge", "exec", "execute",
    "create", "alter", "drop", "truncate", "grant", "deny", "revoke",
    "openrowset", "openquery", "openjson", "openxml", "bulk",
    "set", "use", "begin", "commit", "rollback", "save",
    "tran", "transaction", "cursor", "declare", "print",
    "raiserror", "throw", "waitfor", "kill", "shutdown",
    "reconfigure", "revert", "grant", "readtext", "writetext",
    "updatetext", "checkpoint", "dbcc", "backup", "restore",
    NULL
};

int vms_tsql_validate_query(const wchar_t* query, char* err_msg, size_t err_cap)
{
    VmsLexer lx;
    VmsToken tok;
    int first_word_ok = 0;
    int words = 0;

    if (err_msg && err_cap) err_msg[0] = 0;
    if (!query || !query[0]) {
        if (err_msg) _snprintf_s(err_msg, err_cap, _TRUNCATE, "empty query");
        return 0;
    }
    if (wcslen(query) > 32768) {
        if (err_msg) _snprintf_s(err_msg, err_cap, _TRUNCATE, "query too long");
        return 0;
    }

    vms_lexer_init(&lx, query);
    if (!vms_lexer_next(&lx, &tok)) {
        if (err_msg) _snprintf_s(err_msg, err_cap, _TRUNCATE, "lex error at start");
        return 0;
    }

    /* first meaningful token must be SELECT or WITH (a leading paren is
     * also allowed: (SELECT ...) subquery form) */
    if (tok.kind == VMS_TOK_PUNCT && tok.text[0] == L'(') {
        first_word_ok = 1;
    } else if (word_is(&tok, "select") || word_is(&tok, "with")) {
        first_word_ok = 1;
    }
    if (!first_word_ok) {
        if (err_msg) _snprintf_s(err_msg, err_cap, _TRUNCATE,
                                 "query must start with SELECT or WITH");
        return 0;
    }
    words++;

    for (;;) {
        if (!vms_lexer_next(&lx, &tok)) {
            if (err_msg) _snprintf_s(err_msg, err_cap, _TRUNCATE,
                                     "lex error (unterminated string/comment or bad char)");
            return 0;
        }
        if (tok.kind == VMS_TOK_END) break;
        words++;

        if (tok.kind == VMS_TOK_WORD) {
            const char** f;
            for (f = k_forbidden; *f; f++) {
                if (word_is(&tok, *f)) {
                    if (err_msg) _snprintf_s(err_msg, err_cap, _TRUNCATE,
                                             "forbidden keyword '%s'", *f);
                    return 0;
                }
            }
            if (word_is(&tok, "into")) {
                /* SELECT ... INTO is a table-creating form */
                if (err_msg) _snprintf_s(err_msg, err_cap, _TRUNCATE,
                                         "forbidden keyword 'into'");
                return 0;
            }
        }
        if (tok.kind == VMS_TOK_PUNCT && tok.text[0] == L';') {
            /* statement separator: only a trailing semicolon is allowed */
            VmsToken nxt;
            if (!vms_lexer_next(&lx, &nxt)) {
                if (err_msg) _snprintf_s(err_msg, err_cap, _TRUNCATE, "lex error after ';'");
                return 0;
            }
            if (nxt.kind != VMS_TOK_END) {
                if (err_msg) _snprintf_s(err_msg, err_cap, _TRUNCATE,
                                         "multiple statements are not allowed");
                return 0;
            }
            break;
        }
    }

    if (lx.depth != 0) {
        if (err_msg) _snprintf_s(err_msg, err_cap, _TRUNCATE, "unbalanced parentheses");
        return 0;
    }
    if (words < 2) {
        if (err_msg) _snprintf_s(err_msg, err_cap, _TRUNCATE, "query too short");
        return 0;
    }
    return 1;
}
