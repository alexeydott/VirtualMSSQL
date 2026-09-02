/* implementation of the test harness (kept separate so suites that don't need
 * sqlite3 don't link it). Links against the official precompiled sqlite3.dll
 * import library; sqlite3.dll is placed next to the test exe by CMake. */
#include "test_harness.h"
#include <string.h>

int vms_test_load_extension(const char* dll_path, sqlite3** db, char** err)
{
    sqlite3* d = NULL;
    char* errmsg = NULL;
    int rc;

    if (db) *db = NULL;
    if (err) *err = NULL;

    rc = sqlite3_open(":memory:", &d);
    if (rc != SQLITE_OK) {
        if (err) *err = sqlite3_mprintf("open:memory failed: %s", sqlite3_errmsg(d));
        if (d) sqlite3_close(d);
        return rc;
    }
    sqlite3_enable_load_extension(d, 1);
    rc = sqlite3_load_extension(d, dll_path, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        if (err) *err = errmsg;
        else if (errmsg) sqlite3_free(errmsg);
        sqlite3_close(d);
        return rc;
    }
    if (db) *db = d;
    else sqlite3_close(d);
    return SQLITE_OK;
}
