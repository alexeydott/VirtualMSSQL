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
