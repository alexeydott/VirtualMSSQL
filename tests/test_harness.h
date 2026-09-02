/* test harness shared by CTest suites (R1.4).
 * Links the official SQLite DLL so tests exercise real load_extension. */
#ifndef VMS_TEST_HARNESS_H
#define VMS_TEST_HARNESS_H

#include "sqlite3.h"

/* open in-memory db with extension loading enabled and load the DLL.
 * On success returns SQLITE_OK and *db is open with extension loaded.
 * On failure returns non-zero; *err (sqlite3_malloc'ed) holds errmsg. */
int vms_test_load_extension(const char* dll_path, sqlite3** db, char** err);

#endif
