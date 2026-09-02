/* R1.4 CTest suite 1: extension loads and registers stub objects. */
#include "test_harness.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
    sqlite3* db = NULL;
    char* err = NULL;
    const char* dll;
    sqlite3_stmt* st = NULL;
    int rc;

    if (argc < 2) {
        fprintf(stderr, "usage: test_load <virtualmssql.dll path>\n");
        return 2;
    }
    dll = argv[1];

    rc = vms_test_load_extension(dll, &db, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "load failed: %s\n", err ? err : "(no errmsg)");
        if (err) sqlite3_free(err);
        if (db) sqlite3_close(db);
        return 1;
    }

    /* stub vtab must be queryable */
    rc = sqlite3_prepare_v2(db, "SELECT msg FROM virtualmssql_stub", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "stub vtab not queryable: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 1;
    }
    if (sqlite3_step(st) != SQLITE_DONE) {
        fprintf(stderr, "stub vtab returned a row (expected none)\n");
        sqlite3_finalize(st);
        sqlite3_close(db);
        return 1;
    }
    sqlite3_finalize(st);

    /* version scalar */
    rc = sqlite3_prepare_v2(db, "SELECT virtualmssql_version()", -1, &st, NULL);
    if (rc != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW) {
        fprintf(stderr, "virtualmssql_version() failed: %s\n", sqlite3_errmsg(db));
        if (st) sqlite3_finalize(st);
        sqlite3_close(db);
        return 1;
    }
    printf("virtualmssql_version() = %s\n", sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    sqlite3_close(db);
    printf("test_load: PASS\n");
    return 0;
}
