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
    src/main/cpp/core/archive/archive_resolver.cpp
    src/main/cpp/core/common/version_compare.cpp
    src/main/cpp/core/config/configuration.cpp
    src/main/cpp/core/download/downloader.cpp
    src/main/cpp/core/download/downloader_core.cpp
    src/main/cpp/core/execution/executor.cpp
    src/main/cpp/core/execution/orchestrator.cpp
    src/main/cpp/core/export/audit_exporter.cpp
    src/main/cpp/core/export/sbom_exporter.cpp
    src/main/cpp/core/export/snapshot_exporter.cpp
    src/main/cpp/core/history/history_manager.cpp
    src/main/cpp/core/host/host_info.cpp
    src/main/cpp/core/manifest/manifest_loader.cpp
    src/main/cpp/core/packages/rq_package.cpp
    src/main/cpp/core/packages/rq_repository.cpp
    src/main/cpp/core/planning/planner.cpp
    src/main/cpp/core/planning/planner_core.cpp
    src/main/cpp/core/planning/request_resolution.cpp
    src/main/cpp/core/plugins/plugin_test_runner.cpp
    src/main/cpp/core/registry/registry.cpp
    src/main/cpp/core/registry/registry_database.cpp
    src/main/cpp/core/registry/registry_database_core.cpp
    src/main/cpp/core/registry/registry_json_parser.cpp
    src/main/cpp/core/remote/remote_client.cpp
    src/main/cpp/core/remote/remote_profiles.cpp
    src/main/cpp/core/remote/serve_remote.cpp
    src/main/cpp/core/security/osv_core.cpp
    src/main/cpp/core/security/security_gateway_service.cpp
    src/main/cpp/core/security/validator.cpp
    src/main/cpp/core/security/validator_core.cpp
    src/main/cpp/core/security/vulnerability_database.cpp
    src/main/cpp/core/security/vulnerability_matcher.cpp
    src/main/cpp/core/security/vulnerability_sync_service.cpp
    src/main/cpp/core/state/rqp_state_store.cpp
    src/main/cpp/core/state/transaction_database.cpp
    src/main/cpp/core/state/transaction_database_core.cpp
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
    tests/integration/core/execution/executor_service_tests.cpp
    tests/integration/core/execution/orchestrator_service_tests.cpp
    tests/integration/core/plugins/plugin_test_runner_service_tests.cpp
    tests/integration/core/planning/planner_service_tests.cpp
    tests/integration/core/registry/registry_service_tests.cpp
    tests/integration/plugins/lua_bridge/service_tests.cpp
    src/main/cpp/core/execution/executor.cpp
    src/main/cpp/core/host/host_info.cpp
    src/main/cpp/core/security/osv_core.cpp
    src/main/cpp/core/planning/planner.cpp
    src/main/cpp/core/planning/planner_core.cpp
    src/main/cpp/core/planning/request_resolution.cpp
    src/main/cpp/core/security/security_gateway_service.cpp
    src/main/cpp/core/export/audit_exporter.cpp
    src/main/cpp/core/export/sbom_exporter.cpp
    src/main/cpp/core/export/snapshot_exporter.cpp
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
    src/main/cpp/core/archive/archive_resolver.cpp
    src/main/cpp/core/common/version_compare.cpp
    src/main/cpp/core/config/configuration.cpp
    src/main/cpp/core/download/downloader.cpp
    src/main/cpp/core/download/downloader_core.cpp
    src/main/cpp/core/history/history_manager.cpp
    src/main/cpp/core/packages/rq_package.cpp
    src/main/cpp/core/packages/rq_repository.cpp
    src/main/cpp/core/plugins/plugin_test_runner.cpp
    src/main/cpp/core/registry/registry.cpp
    src/main/cpp/core/registry/registry_database.cpp
    src/main/cpp/core/registry/registry_database_core.cpp
    src/main/cpp/core/registry/registry_json_parser.cpp
    src/main/cpp/core/security/vulnerability_database.cpp
    src/main/cpp/core/security/vulnerability_matcher.cpp
    src/main/cpp/core/security/vulnerability_sync_service.cpp
    src/main/cpp/core/state/rqp_state_store.cpp
    src/main/cpp/core/state/transaction_database.cpp
    src/main/cpp/core/state/transaction_database_core.cpp
)

add_dependencies(core_integration_tests ReqPack)

add_executable(core_unit_tests
    tests/cpp/test_main.cpp
    tests/unit/core/common/version_compare_tests.cpp
    tests/unit/core/config/configuration_cli_tests.cpp
    tests/unit/core/config/configuration_parse_tests.cpp
    tests/unit/core/download/downloader_tests.cpp
    tests/unit/core/export/audit_exporter_tests.cpp
    tests/unit/core/export/sbom_exporter_tests.cpp
    tests/unit/core/history/history_manager_tests.cpp
    tests/unit/core/host/host_info_tests.cpp
    tests/unit/core/manifest/manifest_loader_tests.cpp
    tests/unit/core/packages/rq_package_tests.cpp
    tests/unit/core/packages/rq_repository_tests.cpp
    tests/unit/core/planning/planner_core_tests.cpp
    tests/unit/core/registry/registry_database_tests.cpp
    tests/unit/core/registry/registry_json_parser_tests.cpp
    tests/unit/core/security/osv_core_tests.cpp
    tests/unit/core/security/security_gateway_service_tests.cpp
    tests/unit/core/security/validator_policy_tests.cpp
    tests/unit/core/security/validator_validate_tests.cpp
    tests/unit/core/security/vulnerability_database_tests.cpp
    tests/unit/core/state/rqp_state_store_tests.cpp
    tests/unit/core/state/transaction_database_serialization_tests.cpp
    tests/unit/output/logger/format_tests.cpp
    src/main/cpp/core/common/version_compare.cpp
    src/main/cpp/core/config/configuration.cpp
    src/main/cpp/core/host/host_info.cpp
    src/main/cpp/cli/cli.cpp
    src/main/cpp/core/archive/archive_resolver.cpp
    src/main/cpp/core/manifest/manifest_loader.cpp
    src/main/cpp/core/download/downloader.cpp
    src/main/cpp/core/download/downloader_core.cpp
    src/main/cpp/core/export/audit_exporter.cpp
    src/main/cpp/core/export/sbom_exporter.cpp
    src/main/cpp/core/export/snapshot_exporter.cpp
    src/main/cpp/core/history/history_manager.cpp
    src/main/cpp/core/packages/rq_package.cpp
    src/main/cpp/core/packages/rq_repository.cpp
    src/main/cpp/core/planning/planner_core.cpp
    src/main/cpp/core/registry/registry_database.cpp
    src/main/cpp/core/registry/registry_database_core.cpp
    src/main/cpp/core/registry/registry_json_parser.cpp
    src/main/cpp/core/remote/remote_profiles.cpp
    src/main/cpp/core/security/osv_core.cpp
    src/main/cpp/core/security/security_gateway_service.cpp
    src/main/cpp/core/security/validator.cpp
    src/main/cpp/core/security/validator_core.cpp
    src/main/cpp/core/security/vulnerability_database.cpp
    src/main/cpp/core/security/vulnerability_matcher.cpp
    src/main/cpp/core/security/vulnerability_sync_service.cpp
    src/main/cpp/core/state/rqp_state_store.cpp
    src/main/cpp/core/state/transaction_database_core.cpp
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
