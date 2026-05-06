cmake_minimum_required(VERSION 3.15)
project(ReqPack LANGUAGES CXX)
enable_language(C)
enable_testing()

option(REQPACK_ENABLE_COVERAGE "Build ReqPack targets with coverage instrumentation" OFF)
option(REQPACK_ENABLE_PROFILING "Build ReqPack targets with profiling instrumentation" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

function(reqpack_apply_instrumentation target_name)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        if(REQPACK_ENABLE_COVERAGE OR REQPACK_ENABLE_PROFILING)
            message(WARNING "ReqPack instrumentation flags are only configured for GNU or Clang compilers")
        endif()
        return()
    endif()

    if(REQPACK_ENABLE_COVERAGE)
        target_compile_options(${target_name} PRIVATE -O0 -g --coverage)
        target_link_options(${target_name} PRIVATE --coverage)
    endif()

    if(REQPACK_ENABLE_PROFILING)
        if(NOT REQPACK_ENABLE_COVERAGE)
            target_compile_options(${target_name} PRIVATE -O2)
        endif()
        target_compile_options(${target_name} PRIVATE -g -fno-omit-frame-pointer -pg)
        target_link_options(${target_name} PRIVATE -pg)
    endif()
endfunction()

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v2.13.10
)

find_package(Lua REQUIRED)
find_package(CLI11 REQUIRED)
find_package(CURL REQUIRED)
find_package(Boost REQUIRED)
find_package(OpenSSL REQUIRED)
find_library(REQPACK_ZSTD_LIBRARY NAMES zstd REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

if(APPLE)
    add_compile_options(-Wno-unknown-warning-option)
endif()

set(REQPACK_PLATFORM_LIBS)
if(UNIX AND NOT APPLE)
    list(APPEND REQPACK_PLATFORM_LIBS util)
endif()

FetchContent_Declare(
    lmdb
    GIT_REPOSITORY https://github.com/LMDB/lmdb.git
    GIT_TAG LMDB_0.9.33
)
FetchContent_MakeAvailable(lmdb)
FetchContent_MakeAvailable(Catch2)

add_library(lmdb STATIC
    ${lmdb_SOURCE_DIR}/libraries/liblmdb/mdb.c
    ${lmdb_SOURCE_DIR}/libraries/liblmdb/midl.c
)

set_target_properties(lmdb PROPERTIES LINKER_LANGUAGE C)
target_include_directories(lmdb PUBLIC ${lmdb_SOURCE_DIR}/libraries/liblmdb)

find_path(SOL2_INCLUDE_DIR 
    NAMES sol/sol.hpp
    PATHS /usr/include /usr/local/include
)

if(NOT SOL2_INCLUDE_DIR)
    message(FATAL_ERROR "Sol2 Header nicht gefunden!")
endif()

include_directories(src/main/include)
include_directories(src/main/include/core)
include_directories(src/main/include/plugins)
include_directories(src/main/include/cli)
include_directories(src/main/include/output)

set(SOURCES
    src/main/cpp/main.cpp
    src/main/cpp/cli/cli.cpp
    src/main/cpp/output/logger.cpp
    src/main/cpp/output/logger_core.cpp
    src/main/cpp/output/command_output.cpp
    src/main/cpp/output/ansi_color.cpp
    src/main/cpp/output/plain_display.cpp
    src/main/cpp/output/color_display.cpp
    src/main/cpp/output/display_factory.cpp
    src/main/cpp/plugins/exec_rules.cpp
    src/main/cpp/plugins/exec_rules_core.cpp
    src/main/cpp/plugins/lua_bridge.cpp
    src/main/cpp/plugins/rq_plugin.cpp
    src/main/cpp/core/configuration.cpp
    src/main/cpp/core/host_info.cpp
    src/main/cpp/core/manifest_loader.cpp
    src/main/cpp/core/archive_resolver.cpp
    src/main/cpp/core/downloader.cpp
    src/main/cpp/core/downloader_core.cpp
    src/main/cpp/core/registry_database.cpp
    src/main/cpp/core/registry_database_core.cpp
    src/main/cpp/core/registry_json_parser.cpp
    src/main/cpp/core/rq_package.cpp
    src/main/cpp/core/rq_repository.cpp
    src/main/cpp/core/rqp_state_store.cpp
    src/main/cpp/core/transaction_database.cpp
    src/main/cpp/core/transaction_database_core.cpp
    src/main/cpp/core/orchestrator.cpp
    src/main/cpp/core/osv_core.cpp
    src/main/cpp/core/planner.cpp
    src/main/cpp/core/planner_core.cpp
    src/main/cpp/core/request_resolution.cpp
    src/main/cpp/core/security_gateway_service.cpp
    src/main/cpp/core/audit_exporter.cpp
    src/main/cpp/core/sbom_exporter.cpp
    src/main/cpp/core/snapshot_exporter.cpp
    src/main/cpp/core/validator.cpp
    src/main/cpp/core/validator_core.cpp
    src/main/cpp/core/version_compare.cpp
    src/main/cpp/core/vulnerability_database.cpp
    src/main/cpp/core/vulnerability_matcher.cpp
    src/main/cpp/core/vulnerability_sync_service.cpp
    src/main/cpp/core/serve_remote.cpp
    src/main/cpp/core/remote_profiles.cpp
    src/main/cpp/core/remote_client.cpp
    src/main/cpp/core/executor.cpp
    src/main/cpp/core/history_manager.cpp
    src/main/cpp/core/registry.cpp
)

add_executable(ReqPack ${SOURCES})
set_target_properties(ReqPack PROPERTIES OUTPUT_NAME rqp)

add_executable(exec_rules_unit_tests
    tests/cpp/test_main.cpp
    tests/unit/plugins/exec_rules/parse_tests.cpp
    tests/unit/plugins/exec_rules/evaluation_tests.cpp
    tests/unit/plugins/exec_rules/normalize_tests.cpp
    src/main/cpp/plugins/exec_rules_core.cpp
)

add_executable(exec_rules_integration_tests
    tests/cpp/test_main.cpp
    tests/integration/plugins/exec_rules/smoke_tests.cpp
    src/main/cpp/plugins/exec_rules_core.cpp
)

add_executable(core_integration_tests
    tests/cpp/test_main.cpp
    tests/integration/core/executor/service_tests.cpp
    tests/integration/core/orchestrator/service_tests.cpp
    tests/integration/core/planner/service_tests.cpp
    tests/integration/core/registry/service_tests.cpp
    tests/integration/plugins/lua_bridge/service_tests.cpp
    src/main/cpp/core/executor.cpp
    src/main/cpp/core/host_info.cpp
    src/main/cpp/core/osv_core.cpp
    src/main/cpp/core/planner.cpp
    src/main/cpp/core/planner_core.cpp
    src/main/cpp/core/request_resolution.cpp
    src/main/cpp/core/security_gateway_service.cpp
    src/main/cpp/core/audit_exporter.cpp
    src/main/cpp/core/sbom_exporter.cpp
    src/main/cpp/core/snapshot_exporter.cpp
    src/main/cpp/output/logger.cpp
    src/main/cpp/output/logger_core.cpp
    src/main/cpp/output/command_output.cpp
    src/main/cpp/output/ansi_color.cpp
    src/main/cpp/output/plain_display.cpp
    src/main/cpp/output/color_display.cpp
    src/main/cpp/output/display_factory.cpp
    src/main/cpp/plugins/exec_rules.cpp
    src/main/cpp/plugins/exec_rules_core.cpp
    src/main/cpp/plugins/lua_bridge.cpp
    src/main/cpp/plugins/rq_plugin.cpp
    src/main/cpp/core/configuration.cpp
    src/main/cpp/core/archive_resolver.cpp
    src/main/cpp/core/downloader.cpp
    src/main/cpp/core/downloader_core.cpp
    src/main/cpp/core/rq_package.cpp
    src/main/cpp/core/rq_repository.cpp
    src/main/cpp/core/rqp_state_store.cpp
    src/main/cpp/core/registry.cpp
    src/main/cpp/core/transaction_database.cpp
    src/main/cpp/core/transaction_database_core.cpp
    src/main/cpp/core/registry_database.cpp
    src/main/cpp/core/registry_database_core.cpp
    src/main/cpp/core/registry_json_parser.cpp
    src/main/cpp/core/history_manager.cpp
    src/main/cpp/core/version_compare.cpp
    src/main/cpp/core/vulnerability_database.cpp
    src/main/cpp/core/vulnerability_matcher.cpp
    src/main/cpp/core/vulnerability_sync_service.cpp
)

add_dependencies(core_integration_tests ReqPack)

add_executable(core_unit_tests
    tests/cpp/test_main.cpp
    tests/unit/core/configuration/parse_tests.cpp
    tests/unit/core/configuration/cli_tests.cpp
    tests/unit/core/host_info/core_tests.cpp
    tests/unit/core/downloader/core_tests.cpp
    tests/unit/core/history/core_tests.cpp
    tests/unit/core/manifest_loader/core_tests.cpp
    tests/unit/core/planner/core_tests.cpp
    tests/unit/core/registry_database/core_tests.cpp
    tests/unit/core/registry_json_parser/core_tests.cpp
    tests/unit/core/rq_package/core_tests.cpp
    tests/unit/core/rq_repository/core_tests.cpp
    tests/unit/core/rqp_state_store/core_tests.cpp
    tests/unit/core/osv/core_tests.cpp
    tests/unit/core/security_gateway_service/core_tests.cpp
    tests/unit/core/audit/export_tests.cpp
    tests/unit/core/sbom/export_tests.cpp
    tests/unit/core/transaction_database/serialization_tests.cpp
    tests/unit/core/validator/policy_tests.cpp
    tests/unit/core/validator/validate_tests.cpp
    tests/unit/core/version_compare/core_tests.cpp
    tests/unit/core/vulnerability_database/core_tests.cpp
    tests/unit/output/logger/format_tests.cpp
    src/main/cpp/core/configuration.cpp
    src/main/cpp/core/host_info.cpp
    src/main/cpp/cli/cli.cpp
    src/main/cpp/core/archive_resolver.cpp
    src/main/cpp/core/manifest_loader.cpp
    src/main/cpp/core/downloader.cpp
    src/main/cpp/core/downloader_core.cpp
    src/main/cpp/core/osv_core.cpp
    src/main/cpp/core/planner_core.cpp
    src/main/cpp/core/rq_package.cpp
    src/main/cpp/core/rq_repository.cpp
    src/main/cpp/core/rqp_state_store.cpp
    src/main/cpp/core/registry_database.cpp
    src/main/cpp/core/registry_database_core.cpp
    src/main/cpp/core/registry_json_parser.cpp
    src/main/cpp/core/security_gateway_service.cpp
    src/main/cpp/core/audit_exporter.cpp
    src/main/cpp/core/sbom_exporter.cpp
    src/main/cpp/core/snapshot_exporter.cpp
    src/main/cpp/core/history_manager.cpp
    src/main/cpp/core/transaction_database_core.cpp
    src/main/cpp/core/validator.cpp
    src/main/cpp/core/validator_core.cpp
    src/main/cpp/core/version_compare.cpp
    src/main/cpp/core/vulnerability_database.cpp
    src/main/cpp/core/vulnerability_matcher.cpp
    src/main/cpp/core/vulnerability_sync_service.cpp
    src/main/cpp/core/remote_profiles.cpp
    src/main/cpp/output/ansi_color.cpp
    src/main/cpp/output/logger.cpp
    src/main/cpp/output/logger_core.cpp
    src/main/cpp/output/plain_display.cpp
    src/main/cpp/output/color_display.cpp
    src/main/cpp/output/command_output.cpp
)

add_custom_target(reqpack_test_targets
    DEPENDS
        core_unit_tests
        core_integration_tests
        exec_rules_unit_tests
        exec_rules_integration_tests
)

foreach(reqpack_target IN ITEMS
    ReqPack
    exec_rules_unit_tests
    exec_rules_integration_tests
    core_unit_tests
    core_integration_tests
)
    reqpack_apply_instrumentation(${reqpack_target})
endforeach()

target_include_directories(ReqPack PRIVATE 
    ${SOL2_INCLUDE_DIR}
    ${LUA_INCLUDE_DIR}
    src/main/include
)

target_include_directories(exec_rules_unit_tests PRIVATE
    ${SOL2_INCLUDE_DIR}
    ${LUA_INCLUDE_DIR}
    src/main/include
    tests/cpp
)

target_include_directories(exec_rules_integration_tests PRIVATE
    ${SOL2_INCLUDE_DIR}
    ${LUA_INCLUDE_DIR}
    src/main/include
    tests/cpp
)

target_include_directories(core_unit_tests PRIVATE
    ${SOL2_INCLUDE_DIR}
    ${LUA_INCLUDE_DIR}
    src/main/include
    tests/cpp
)

target_include_directories(core_integration_tests PRIVATE
    ${SOL2_INCLUDE_DIR}
    ${LUA_INCLUDE_DIR}
    src/main/include
    tests/cpp
)

target_link_libraries(ReqPack PRIVATE 
	CLI11::CLI11
	CURL::libcurl
	lmdb
	OpenSSL::Crypto
	${REQPACK_PLATFORM_LIBS}
	${REQPACK_ZSTD_LIBRARY}
	${LUA_LIBRARIES}
	Boost::boost
	spdlog::spdlog
	fmt::fmt
)

target_link_libraries(exec_rules_unit_tests PRIVATE
    Catch2::Catch2
    ${LUA_LIBRARIES}
    spdlog::spdlog
    fmt::fmt
)

target_link_libraries(exec_rules_integration_tests PRIVATE
    Catch2::Catch2
    ${LUA_LIBRARIES}
    spdlog::spdlog
    fmt::fmt
)

target_link_libraries(core_unit_tests PRIVATE
    Catch2::Catch2
    CLI11::CLI11
    CURL::libcurl
    lmdb
    OpenSSL::Crypto
	${REQPACK_PLATFORM_LIBS}
    ${REQPACK_ZSTD_LIBRARY}
    ${LUA_LIBRARIES}
    Boost::boost
    spdlog::spdlog
    fmt::fmt
)

target_link_libraries(core_integration_tests PRIVATE
    Catch2::Catch2
    CURL::libcurl
    lmdb
    OpenSSL::Crypto
	${REQPACK_PLATFORM_LIBS}
    ${REQPACK_ZSTD_LIBRARY}
    ${LUA_LIBRARIES}
    Boost::boost
    spdlog::spdlog
    fmt::fmt
)

target_compile_definitions(ReqPack PRIVATE SOL_ALL_SAFETIES_ON=1)
target_compile_definitions(exec_rules_unit_tests PRIVATE
    SOL_ALL_SAFETIES_ON=1
    REQPACK_TEST_REPO_ROOT="${CMAKE_SOURCE_DIR}"
    REQPACK_TEST_BUILD_DIR="${CMAKE_BINARY_DIR}"
)
target_compile_definitions(exec_rules_integration_tests PRIVATE
    SOL_ALL_SAFETIES_ON=1
    REQPACK_TEST_REPO_ROOT="${CMAKE_SOURCE_DIR}"
    REQPACK_TEST_BUILD_DIR="${CMAKE_BINARY_DIR}"
)
target_compile_definitions(core_unit_tests PRIVATE
    SOL_ALL_SAFETIES_ON=1
    REQPACK_TEST_REPO_ROOT="${CMAKE_SOURCE_DIR}"
    REQPACK_TEST_BUILD_DIR="${CMAKE_BINARY_DIR}"
)
target_compile_definitions(core_integration_tests PRIVATE
    SOL_ALL_SAFETIES_ON=1
    REQPACK_TEST_REPO_ROOT="${CMAKE_SOURCE_DIR}"
    REQPACK_TEST_BUILD_DIR="${CMAKE_BINARY_DIR}"
)

include(CTest)
include(${catch2_SOURCE_DIR}/contrib/Catch.cmake)
catch_discover_tests(exec_rules_unit_tests TEST_PREFIX "unit::")
catch_discover_tests(exec_rules_integration_tests TEST_PREFIX "integration::")
catch_discover_tests(core_unit_tests TEST_PREFIX "unit::")
catch_discover_tests(core_integration_tests TEST_PREFIX "integration::")
