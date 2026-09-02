/* example: load virtualmssql.dll into an in-memory database and smoke it.
 * Build with the same include paths as the tests (see examples/CMakeLists.txt). */
#include "sqlite3.h"
#include <stdio.h>

int main(int argc, char** argv)
{
    sqlite3* db = NULL;
    char* err = NULL;
    sqlite3_stmt* st = NULL;
    const char* dll;
    int rc;

    if (argc < 2) {
        fprintf(stderr, "usage: load_smoke <virtualmssql.dll path>\n");
        return 2;
    }
    dll = argv[1];

    rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "open failed\n");
        return 1;
    }
    sqlite3_enable_load_extension(db, 1);
    rc = sqlite3_load_extension(db, dll, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "load failed: %s\n", err ? err : "?");
        if (err) sqlite3_free(err);
        sqlite3_close(db);
        return 1;
    }
    rc = sqlite3_prepare_v2(db, "SELECT virtualmssql_version()", -1, &st, NULL);
    if (rc == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        printf("loaded: %s\n", sqlite3_column_text(st, 0));
    }
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return rc == SQLITE_OK ? 0 : 1;
}
