/* R1.4 CTest suite 2: both export symbols resolve via GetProcAddress. */
#include <windows.h>
#include <stdio.h>

int main(int argc, char** argv)
{
    HMODULE h;
    FARPROC p1, p2;

    if (argc < 2) {
        fprintf(stderr, "usage: test_exports <virtualmssql.dll path>\n");
        return 2;
    }
    h = LoadLibraryA(argv[1]);
    if (!h) {
        fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    p1 = GetProcAddress(h, "sqlite3_virtualmssql_init");
    p2 = GetProcAddress(h, "sqlite3_extension_init");
    if (!p1 || !p2) {
        fprintf(stderr, "missing export: sqlite3_virtualmssql_init=%p sqlite3_extension_init=%p\n",
                (void*)p1, (void*)p2);
        FreeLibrary(h);
        return 1;
    }
    FreeLibrary(h);
    printf("test_exports: PASS\n");
    return 0;
}
