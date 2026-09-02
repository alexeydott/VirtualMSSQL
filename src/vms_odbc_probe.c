/* ODBC adapter layer — driver 18 presence probe (R1 stub).
 * ODBC types are confined to this adapter layer per TZ. */
#include "vms_internal.h"
#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#include <sqlucode.h>
#include <string.h>
#include <stdio.h>

int vms_driver18_present(void)
{
    SQLHENV env = SQL_NULL_HENV;
    SQLWCHAR name[512];
    SQLWCHAR attrs[1024];
    SQLSMALLINT name_len = 0, attrs_len = 0;
    SQLRETURN r;
    int found = 0;
    char name_u8[512];

    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env))) {
        return 0;
    }
    SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3_80, 0);

    r = SQLDriversW(env, SQL_FETCH_FIRST, name,
                    (SQLSMALLINT)(sizeof(name) / sizeof(name[0])), &name_len,
                    attrs, (SQLSMALLINT)(sizeof(attrs) / sizeof(attrs[0])), &attrs_len);
    while (SQL_SUCCEEDED(r) && !found) {
        WideCharToMultiByte(CP_UTF8, 0, (LPCWCH)name, -1, name_u8,
                            sizeof(name_u8), NULL, NULL);
        if (strstr(name_u8, "ODBC Driver 18 for SQL Server") != NULL) {
            found = 1;
        }
        r = SQLDriversW(env, SQL_FETCH_NEXT, name,
                        (SQLSMALLINT)(sizeof(name) / sizeof(name[0])), &name_len,
                        attrs, (SQLSMALLINT)(sizeof(attrs) / sizeof(attrs[0])), &attrs_len);
    }
    SQLFreeHandle(SQL_HANDLE_ENV, env);
    return found;
}

const char* vms_error_class_driver_probe(int driver_found)
{
    return driver_found ? "OK" : "DRIVER_NOT_FOUND";
}
