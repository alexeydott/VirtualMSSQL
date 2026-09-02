cmake_minimum_required(VERSION 3.20)
project(vms-odbc-probe VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(vms-odbc-probe
    src/main.c
    src/util.c
    src/diag.c
    src/env.c
    src/report.c
    src/cases_connect.c
    src/cases_exec.c
    src/cases_stream.c
    src/cases_cancel.c
    src/cases_tx.c
)

target_include_directories(vms-odbc-probe PRIVATE src)
target_link_libraries(vms-odbc-probe PRIVATE odbc32)

if(MSVC)
    target_compile_options(vms-odbc-probe PRIVATE /W4 /permissive-)
    target_compile_definitions(vms-odbc-probe PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()
