# R1.4 test suites. Tests link the precompiled official sqlite3 import lib
# (sqlite3.lib built from the official .def) so load_extension is real.

set(SQLITE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sqlite")

# official precompiled DLLs are copied from third_party/bin/<arch>
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(VMS_ARCH x64)
else()
    set(VMS_ARCH x86)
endif()
set(VMS_SQLITE_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/bin/${VMS_ARCH}")

# place the host sqlite3.dll next to every test executable that links it
file(COPY "${VMS_SQLITE_BIN_DIR}/sqlite3.dll" DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

# ---- harness implementation (static; linked into tests that need sqlite3) ----
add_library(vms_test_harness STATIC test_harness_impl.c)
target_include_directories(vms_test_harness PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SQLITE_INCLUDE_DIR}
)
target_link_libraries(vms_test_harness PUBLIC
    "${VMS_SQLITE_BIN_DIR}/sqlite3.lib"
)
target_compile_definitions(vms_test_harness PUBLIC
    _CRT_SECURE_NO_WARNINGS
)

# ---- suite 1: load ----
add_executable(test_load test_load.c)
target_include_directories(test_load PRIVATE ${SQLITE_INCLUDE_DIR})
target_link_libraries(test_load PRIVATE vms_test_harness)
add_dependencies(test_load virtualmssql)
add_test(NAME load
    COMMAND test_load $<TARGET_FILE:virtualmssql>)
set_tests_properties(load PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${VMS_SQLITE_BIN_DIR}")

# ---- suite 2: exports ----
add_executable(test_exports test_exports.c)
add_dependencies(test_exports virtualmssql)
add_test(NAME exports
    COMMAND test_exports $<TARGET_FILE:virtualmssql>)

# ---- suite 3: capability gate ----
add_executable(test_capability test_capability.c
    ../src/vms_capabilities.c)
target_include_directories(test_capability PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${SQLITE_INCLUDE_DIR}
)
target_compile_definitions(test_capability PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(test_capability PRIVATE odbc32)
add_test(NAME capability COMMAND test_capability)

# ---- suite 4: missing-driver diagnostic contract ----
add_executable(test_missing_driver test_missing_driver.c
    ../src/vms_odbc_probe.c)
target_include_directories(test_missing_driver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${SQLITE_INCLUDE_DIR}
)
target_compile_definitions(test_missing_driver PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(test_missing_driver PRIVATE odbc32)
add_test(NAME missing_driver COMMAND test_missing_driver)

# ---- suite 5: foundation core (R2 gate) ----
add_executable(test_foundation test_foundation.c
    ../src/vms_foundation.c
    ../src/vms_limits.c
    ../src/vms_log.c)
target_include_directories(test_foundation PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_definitions(test_foundation PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME foundation COMMAND test_foundation)

# ---- suite 6: client runtime integration (G3; needs live SQL Server) ----
add_executable(test_client test_client.c)
target_include_directories(test_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_definitions(test_client PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(test_client PRIVATE /utf-8)
target_link_libraries(test_client PRIVATE vms_client)
add_test(NAME client COMMAND test_client)
set_tests_properties(client PROPERTIES SKIP_RETURN_CODE 77)

# ---- suite 7: credentials/connstr/pool (G4) ----
add_executable(test_r4 test_r4.c)
target_link_libraries(test_r4 PRIVATE vms_client)
target_compile_options(test_r4 PRIVATE /utf-8)
add_test(NAME r4 COMMAND test_r4)

# ---- suite 8: metadata/stable key/identity (G5) ----
add_executable(test_r5 test_r5.c)
target_link_libraries(test_r5 PRIVATE vms_client)
target_compile_options(test_r5 PRIVATE /utf-8)
add_test(NAME r5 COMMAND test_r5)

# ---- suite 9: vtab read path (G6; needs live server) ----
add_executable(test_r6 test_r6.c)
target_include_directories(test_r6 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${SQLITE_INCLUDE_DIR}
)
target_link_libraries(test_r6 PRIVATE vms_test_harness vms_client dbghelp)
target_compile_options(test_r6 PRIVATE /utf-8)
add_dependencies(test_r6 virtualmssql)
add_test(NAME r6
    COMMAND test_r6 $<TARGET_FILE:virtualmssql>)

# ---- suite 10: safe pushdown differential (G7; needs live server) ----
add_executable(test_r7 test_r7.c)
target_include_directories(test_r7 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${SQLITE_INCLUDE_DIR}
)
target_link_libraries(test_r7 PRIVATE vms_test_harness vms_client)
target_compile_options(test_r7 PRIVATE /utf-8)
add_dependencies(test_r7 virtualmssql)
add_test(NAME r7
    COMMAND test_r7 $<TARGET_FILE:virtualmssql>)

# ---- suite 11: source=query / query profiles (G8) ----
add_executable(test_r8 test_r8.c)
target_include_directories(test_r8 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${SQLITE_INCLUDE_DIR}
)
target_link_libraries(test_r8 PRIVATE vms_test_harness vms_client)
target_compile_options(test_r8 PRIVATE /utf-8)
add_dependencies(test_r8 virtualmssql)
add_test(NAME r8
    COMMAND test_r8 $<TARGET_FILE:virtualmssql>)

# ---- suite 12: materialization (G9; needs live server) ----
add_executable(test_r9 test_r9.c)
target_include_directories(test_r9 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${SQLITE_INCLUDE_DIR}
)
target_link_libraries(test_r9 PRIVATE vms_test_harness vms_client)
target_compile_options(test_r9 PRIVATE /utf-8)
add_dependencies(test_r9 virtualmssql)
add_test(NAME r9
    COMMAND test_r9 $<TARGET_FILE:virtualmssql>)

# seed helper (not a test): creates R6 fixtures on the live server
add_executable(seed_r6 seed_r6.c)
target_link_libraries(seed_r6 PRIVATE vms_client)
target_compile_options(seed_r6 PRIVATE /utf-8)
