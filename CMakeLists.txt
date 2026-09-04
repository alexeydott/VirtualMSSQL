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

# R16: opt-in MSVC static analysis (/analyze + /W3 warnings-as-errors on
# analysis codes); enabled via -DVMS_ANALYZE=ON configure
option(VMS_ANALYZE "Enable MSVC /analyze static analysis" OFF)
# R16: clang-cl ASan/UBSan instrumentation for the asan-x64 preset
option(VMS_ASAN "Enable AddressSanitizer + UBSan (clang-cl toolchain)" OFF)
# the dynamic ASan runtime DLL must sit next to every instrumented binary
# (MSVC "one DLL for all runtime configurations")
set(VMS_ASAN_DLL "D:/Visual Studio2022/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/clang_rt.asan_dynamic-x86_64.dll"
    CACHE FILEPATH "Path to clang_rt.asan_dynamic-x86_64.dll")

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
    src/vms_plan.c
    src/vms_lexer.c
    src/vms_query_source.c
    src/vms_mat.c
    src/vms_dml.c
    src/vms_meta_cache.c
    src/vms_identity.c
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
target_link_libraries(vms_client PUBLIC odbc32 advapi32
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bin/$<IF:$<EQUAL:${CMAKE_SIZEOF_VOID_P},8>,x64,x86>/sqlite3.lib")
# vms_mat.c / vms_meta_cache.c are SQLITE_CORE (direct sqlite3 use for the
# private snapshot / metadata cache dbs)
set_source_files_properties(src/vms_mat.c src/vms_meta_cache.c PROPERTIES COMPILE_DEFINITIONS "SQLITE_CORE")
target_compile_options(vms_client PRIVATE /W4 /utf-8)
if(VMS_ANALYZE)
    target_compile_options(vms_client PRIVATE
        /analyze /analyze:external- /wd28252 /wd28253)
    target_compile_definitions(vms_client PRIVATE VMS_ANALYZE=1)
endif()
if(VMS_ASAN)
    # clang-cl: address+undefined sanitizers; /MDd is incompatible with
    # ASan instrumentation, switch to the release CRT. The runtime libs are
    # linked explicitly (lld-link does not pick them automatically).
    set(VMS_ASAN_RT "D:/Visual Studio2022/VC/Tools/Llvm/x64/lib/clang/19/lib/windows")
    target_compile_options(vms_client PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer /O1 /MD)
    target_link_options(vms_client PRIVATE
        -fsanitize=address,undefined
        "${VMS_ASAN_RT}/clang_rt.asan-x86_64.lib"
        "${VMS_ASAN_RT}/clang_rt.ubsan_standalone-x86_64.lib")
endif()

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
if(VMS_ANALYZE)
    target_compile_options(virtualmssql PRIVATE /analyze /analyze:external-)
endif()
if(VMS_ASAN)
    target_compile_options(virtualmssql PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer /O1 /MD)
    target_link_options(virtualmssql PRIVATE
        -fsanitize=address,undefined
        "${VMS_ASAN_RT}/clang_rt.asan_dynamic-x86_64.lib"
        "${VMS_ASAN_RT}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib")
    add_custom_command(TARGET virtualmssql POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VMS_ASAN_DLL}" $<TARGET_FILE_DIR:virtualmssql>)
endif()

# exports: sqlite3_virtualmssql_init + sqlite3_extension_init
# handled by VMS_EXPORT __declspec(dllexport) in include/virtualmssql/vms_api.h

# ---- tests (R1.4) ----
enable_testing()
add_subdirectory(tests)




