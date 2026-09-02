cmake_minimum_required(VERSION 3.20)
project(VirtualMSSQL LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT WIN32)
    message(FATAL_ERROR "VirtualMSSQL targets Windows only (Win32 + x64, MSVC)")
endif()

find_package(SQLite3 QUIET)
set(VMS_SQLITE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite")

# ---- vms_client (R3/R4 runtime): static lib for tests + linked into DLL ----
add_library(vms_client STATIC
    src/vms_odbc_adapter.c
    src/vms_odbc_worker.c
    src/vms_foundation.c
    src/vms_limits.c
    src/vms_log.c
    src/vms_faultalloc.c
    src/vms_credentials.c
    src/vms_connstr.c
    src/vms_pool.c
    src/vms_meta.c
    src/vms_vtab.c
)
target_include_directories(vms_client PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite
)
target_compile_definitions(vms_client PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)
target_link_libraries(vms_client PUBLIC odbc32 advapi32)
target_compile_options(vms_client PRIVATE /W4 /utf-8)

# ---- virtualmssql.dll (loadable extension) ----
add_library(virtualmssql SHARED
    src/vms_extension.c
    src/vms_capabilities.c
    src/vms_odbc_probe.c
)
target_link_libraries(virtualmssql PRIVATE vms_client)
target_include_directories(virtualmssql
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include/virtualmssql
        ${VMS_SQLITE_INCLUDE_DIR}
)
target_compile_definitions(virtualmssql PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)
target_compile_options(virtualmssql PRIVATE /W4)

# exports: sqlite3_virtualmssql_init + sqlite3_extension_init
# handled by VMS_EXPORT __declspec(dllexport) in include/virtualmssql/vms_api.h

# ---- tests (R1.4) ----
enable_testing()
add_subdirectory(tests)
