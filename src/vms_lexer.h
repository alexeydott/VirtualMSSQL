/* vms_lexer.h — bounded T-SQL lexer and query-source validator (R8).
 *
 * Defense in depth: the validator is not the security boundary (the server
 * principal is), but it deterministically rejects everything that is not a
 * plain read-only SELECT / WITH...SELECT statement. */
#ifndef VIRTUALMSSQL_VMS_LEXER_H
#define VIRTUALMSSQL_VMS_LEXER_H

#include <stddef.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum VmsTokKind {
    VMS_TOK_END = 0,
    VMS_TOK_WORD,       /* identifier or keyword (letters, digits, _, #, @) */
    VMS_TOK_NUMBER,
    VMS_TOK_STRING,     /* N'...' or '...' (value not copied; span only) */
    VMS_TOK_PUNCT,      /* single char: ( ) , ; . * + - / % = < > */
    VMS_TOK_OP2,        /* two-char: <= >= <> != = */
    VMS_TOK_ERROR
} VmsTokKind;

typedef struct VmsToken {
    VmsTokKind kind;
    size_t start;        /* byte offset into the input */
    size_t len;          /* byte length */
    wchar_t text[64];    /* lowercased for words, raw char for punct; 0 for strings */
} VmsToken;

typedef struct VmsLexer {
    const wchar_t* src;
    size_t len;
    size_t pos;
    size_t depth;        /* paren depth */
} VmsLexer;

void vms_lexer_init(VmsLexer* lx, const wchar_t* src);
/* next token; returns 0 on overflow/unterminated string (fatal) */
int vms_lexer_next(VmsLexer* lx, VmsToken* tok);

/* validate a query source:
 *   - exactly one statement, terminated by end (a trailing ';' is allowed)
 *   - starts with SELECT or WITH
 *   - no forbidden keywords anywhere: INSERT/UPDATE/DELETE/MERGE/EXEC/
 *     EXECUTE/CREATE/ALTER/DROP/TRUNCATE/GRANT/DENY/REVOKE/OPENROWSET/
 *     OPENQUERY/OPENJSON/OPENXML/BULK/SET/USE/BEGIN/COMMIT/ROLLBACK/
 *     SAVE/TRAN/TRANSACTION/CURSOR/DECLARE/PRINT/RAISERROR/THROW/WAITFOR/
 *     KILL/SHUTDOWN/RECONFIGURE/XP (as prefix)/SP_EXECUTE... (dynamic SQL)
 *   - no INTO (SELECT ... INTO creates tables)
 *   - balanced parentheses
 *   - no semicolons inside (statement separator) except trailing
 * Returns 1 when the query is acceptable; err_msg filled on refusal. */
int vms_tsql_validate_query(const wchar_t* query, char* err_msg, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* VIRTUALMSSQL_VMS_LEXER_H */
