cmake_minimum_required(VERSION 3.15)
project(ReqPack LANGUAGES CXX)
enable_language(C)
enable_testing()

set(CMAKE_NO_SYSTEM_FROM_IMPORTED ON)

set(REQPACK_RELEASE_ID "v1.3.0" CACHE STRING "Build/release identity shown by rqp version output")

option(REQPACK_ENABLE_COVERAGE "Build ReqPack targets with coverage instrumentation" OFF)
option(REQPACK_ENABLE_PROFILING "Build ReqPack targets with profiling instrumentation" OFF)
option(REQPACK_LINK_STATIC_LUA "Prefer static Lua library when available" OFF)

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

if(NOT REQPACK_GO_EXECUTABLE)
    unset(REQPACK_GO_EXECUTABLE CACHE)
    find_program(REQPACK_GO_EXECUTABLE NAMES go go.exe
        HINTS
            "/opt/homebrew/bin"
            "/usr/local/bin"
            "/usr/bin"
            "$ENV{HOME}/go/bin"
            "/usr/local/go/bin"
    )
endif()
set(REQPACK_GO_EXECUTABLE "${REQPACK_GO_EXECUTABLE}" CACHE FILEPATH "Path to the Go compiler")
if(NOT REQPACK_GO_EXECUTABLE)
    message(FATAL_ERROR "Go compiler not found. Install Go or pass -DREQPACK_GO_EXECUTABLE=/path/to/go")
endif()

set(REQPACK_TRIVY_HELPER_DIR "${CMAKE_BINARY_DIR}/tools/trivydb-exporter")
set(REQPACK_TRIVY_HELPER_PATH "${REQPACK_TRIVY_HELPER_DIR}/trivydb-exporter")

add_custom_command(
    OUTPUT ${REQPACK_TRIVY_HELPER_PATH}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${REQPACK_TRIVY_HELPER_DIR}
    COMMAND ${REQPACK_GO_EXECUTABLE} build -o ${REQPACK_TRIVY_HELPER_PATH} .
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tools/trivydb-exporter
    DEPENDS
        ${CMAKE_SOURCE_DIR}/tools/trivydb-exporter/go.mod
        ${CMAKE_SOURCE_DIR}/tools/trivydb-exporter/go.sum
        ${CMAKE_SOURCE_DIR}/tools/trivydb-exporter/main.go
    COMMENT "Building Trivy DB helper"
    VERBATIM
)

add_custom_target(reqpack_trivy_helper ALL DEPENDS ${REQPACK_TRIVY_HELPER_PATH})

set(REQPACK_LUA_LINK_LIBRARIES ${LUA_LIBRARIES})
if(REQPACK_LINK_STATIC_LUA)
    set(_reqpack_saved_library_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
    set(CMAKE_FIND_LIBRARY_SUFFIXES .a)

    set(_reqpack_lua_hint_dirs)
    foreach(_reqpack_lua_lib IN LISTS LUA_LIBRARIES)
        if(_reqpack_lua_lib)
            get_filename_component(_reqpack_lua_lib_dir "${_reqpack_lua_lib}" DIRECTORY)
            if(_reqpack_lua_lib_dir)
                list(APPEND _reqpack_lua_hint_dirs "${_reqpack_lua_lib_dir}")
            endif()
        endif()
    endforeach()

    find_library(REQPACK_LUA_STATIC_LIBRARY
        NAMES lua5.4 lua-5.4 lua54 lua
        HINTS ${_reqpack_lua_hint_dirs}
    )

    set(CMAKE_FIND_LIBRARY_SUFFIXES ${_reqpack_saved_library_suffixes})

    if(REQPACK_LUA_STATIC_LIBRARY)
        set(REQPACK_LUA_LINK_LIBRARIES ${REQPACK_LUA_STATIC_LIBRARY})
        message(STATUS "Using static Lua library: ${REQPACK_LUA_STATIC_LIBRARY}")
    else()
        message(WARNING "REQPACK_LINK_STATIC_LUA is ON, but no static Lua library was found. Falling back to ${LUA_LIBRARIES}")
    endif()
endif()

set(REQPACK_FMT_TARGET fmt::fmt)
# Prefer the compiled library on Windows/MinGW: header-only pulls format-inl.h and
# hits GCC 16 -Wtemplate-body failures around stdout/stderr in system packages.
if(NOT WIN32 AND TARGET fmt::fmt-header-only)
    set(REQPACK_FMT_TARGET fmt::fmt-header-only)
endif()

set(REQPACK_SPDLOG_TARGET spdlog::spdlog)
if(NOT WIN32 AND TARGET spdlog::spdlog_header_only)
    set(REQPACK_SPDLOG_TARGET spdlog::spdlog_header_only)
endif()

if(APPLE)
    add_compile_options(-Wno-unknown-warning-option)
endif()

# MinGW fmt/spdlog headers fail with undeclared stdout/stderr inside template bodies.
function(reqpack_apply_windows_compile_options target_name)
    if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target_name} PRIVATE -Wno-template-body)
    endif()
endfunction()

set(REQPACK_PLATFORM_LIBS)
if(WIN32)
    list(APPEND REQPACK_PLATFORM_LIBS ws2_32)
endif()
if(UNIX AND NOT APPLE)
    list(APPEND REQPACK_PLATFORM_LIBS util)
endif()

FetchContent_Declare(
    lmdb
    GIT_REPOSITORY https://github.com/LMDB/lmdb.git
    GIT_TAG LMDB_0.9.33
)
FetchContent_Declare(
    cffi_lua
    GIT_REPOSITORY https://github.com/q66/cffi-lua.git
    GIT_TAG v0.2.4
)
FetchContent_MakeAvailable(lmdb)
FetchContent_MakeAvailable(cffi_lua)
FetchContent_MakeAvailable(Catch2)

add_library(lmdb STATIC
    ${lmdb_SOURCE_DIR}/libraries/liblmdb/mdb.c
    ${lmdb_SOURCE_DIR}/libraries/liblmdb/midl.c
)

set_target_properties(lmdb PROPERTIES LINKER_LANGUAGE C)
target_include_directories(lmdb PUBLIC ${lmdb_SOURCE_DIR}/libraries/liblmdb)

set(REQPACK_SECURITY_CORE_REPO
    "https://github.com/Coditary/reqpack-security-core.git"
    CACHE STRING "Git repository for reqpack-security-core when not available locally"
)
set(REQPACK_SECURITY_CORE_TAG
    "main"
    CACHE STRING "Git tag, branch, or commit for reqpack-security-core FetchContent fallback"
)
set(REQPACK_SECURITY_CORE_DIR
    ""
    CACHE PATH "Optional local checkout of reqpack-security-core (skips FetchContent when valid)"
)

set(_reqpack_security_core_local "${CMAKE_CURRENT_SOURCE_DIR}/../reqpack-security-core")
if(REQPACK_SECURITY_CORE_DIR AND EXISTS "${REQPACK_SECURITY_CORE_DIR}/CMakeLists.txt")
    set(REQPACK_SECURITY_CORE_SOURCE_DIR "${REQPACK_SECURITY_CORE_DIR}")
    message(STATUS "Using local reqpack-security-core: ${REQPACK_SECURITY_CORE_SOURCE_DIR}")
elseif(EXISTS "${_reqpack_security_core_local}/CMakeLists.txt")
    set(REQPACK_SECURITY_CORE_SOURCE_DIR "${_reqpack_security_core_local}")
    message(STATUS "Using sibling reqpack-security-core: ${REQPACK_SECURITY_CORE_SOURCE_DIR}")
else()
    message(STATUS "Fetching reqpack-security-core from ${REQPACK_SECURITY_CORE_REPO} (${REQPACK_SECURITY_CORE_TAG})")
    FetchContent_Declare(
        reqpack_security_core
        GIT_REPOSITORY ${REQPACK_SECURITY_CORE_REPO}
        GIT_TAG ${REQPACK_SECURITY_CORE_TAG}
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(reqpack_security_core)
    set(REQPACK_SECURITY_CORE_SOURCE_DIR "${reqpack_security_core_SOURCE_DIR}")
endif()

if(NOT TARGET reqpack_security_core)
    add_subdirectory(
        ${REQPACK_SECURITY_CORE_SOURCE_DIR}
        ${CMAKE_BINARY_DIR}/reqpack-security-core-build
    )
endif()

# libffi backs the vendored cffi-lua module (Lua FFI for non-LuaJIT environments).
find_path(REQPACK_FFI_INCLUDE_DIR
    NAMES ffi.h
    HINTS
        /opt/homebrew/opt/libffi/include
        /usr/local/opt/libffi/include
)
find_library(REQPACK_FFI_LIBRARY
    NAMES ffi libffi
    HINTS
        /opt/homebrew/opt/libffi/lib
        /usr/local/opt/libffi/lib
)
if(NOT REQPACK_FFI_INCLUDE_DIR OR NOT REQPACK_FFI_LIBRARY)
    message(FATAL_ERROR "libffi nicht gefunden! Bitte installieren (Debian/Ubuntu: libffi-dev, Fedora: libffi-devel, MSYS2: mingw-w64-x86_64-libffi, macOS: brew install libffi) oder via -DREQPACK_FFI_INCLUDE_DIR/-DREQPACK_FFI_LIBRARY angeben.")
endif()

set(REQPACK_FFI_LINK_LIBRARY ${REQPACK_FFI_LIBRARY})
if(REQPACK_LINK_STATIC_LUA)
    # Release builds bundle everything feasible; prefer the static libffi
    # archive next to the discovered library so the shipped binary does not
    # gain a new runtime dependency.
    set(_reqpack_saved_library_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
    set(CMAKE_FIND_LIBRARY_SUFFIXES .a)

    get_filename_component(_reqpack_ffi_lib_dir "${REQPACK_FFI_LIBRARY}" DIRECTORY)
    find_library(REQPACK_FFI_STATIC_LIBRARY
        NAMES ffi libffi
        HINTS
            ${_reqpack_ffi_lib_dir}
            /opt/homebrew/opt/libffi/lib
            /usr/local/opt/libffi/lib
    )

    set(CMAKE_FIND_LIBRARY_SUFFIXES ${_reqpack_saved_library_suffixes})

    if(REQPACK_FFI_STATIC_LIBRARY)
        set(REQPACK_FFI_LINK_LIBRARY ${REQPACK_FFI_STATIC_LIBRARY})
        message(STATUS "Using static libffi: ${REQPACK_FFI_STATIC_LIBRARY}")
    else()
        message(WARNING "Static libffi not found; falling back to ${REQPACK_FFI_LIBRARY}")
    endif()
endif()

add_library(lua_cffi STATIC
    ${cffi_lua_SOURCE_DIR}/src/util.cc
    ${cffi_lua_SOURCE_DIR}/src/ffilib.cc
    ${cffi_lua_SOURCE_DIR}/src/parser.cc
    ${cffi_lua_SOURCE_DIR}/src/ast.cc
    ${cffi_lua_SOURCE_DIR}/src/lib.cc
    ${cffi_lua_SOURCE_DIR}/src/ffi.cc
    ${cffi_lua_SOURCE_DIR}/src/main.cc
)
set_target_properties(lua_cffi PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(lua_cffi PRIVATE
    ${cffi_lua_SOURCE_DIR}/src
    ${REQPACK_FFI_INCLUDE_DIR}
    ${LUA_INCLUDE_DIR}
)
if(DEFINED CMAKE_CXX_BYTE_ORDER)
    if(CMAKE_CXX_BYTE_ORDER STREQUAL "BIG_ENDIAN")
        target_compile_definitions(lua_cffi PRIVATE FFI_BIG_ENDIAN)
    else()
        target_compile_definitions(lua_cffi PRIVATE FFI_LITTLE_ENDIAN)
    endif()
elseif(WIN32)
    target_compile_definitions(lua_cffi PRIVATE FFI_LITTLE_ENDIAN)
else()
    include(TestBigEndian)
    test_big_endian(REQPACK_CFFI_BIG_ENDIAN)
    if(REQPACK_CFFI_BIG_ENDIAN)
        target_compile_definitions(lua_cffi PRIVATE FFI_BIG_ENDIAN)
    else()
        target_compile_definitions(lua_cffi PRIVATE FFI_LITTLE_ENDIAN)
    endif()
endif()
target_link_libraries(lua_cffi PUBLIC ${CMAKE_DL_LIBS} ${REQPACK_FFI_LINK_LIBRARY})

find_path(SOL2_INCLUDE_DIR 
    NAMES sol/sol.hpp
    PATHS /usr/include /usr/local/include
)

if(SOL2_INCLUDE_DIR STREQUAL "/usr/include" OR SOL2_INCLUDE_DIR STREQUAL "/usr/local/include")
    set(SOL2_INCLUDE_DIR "")
endif()

if(NOT SOL2_INCLUDE_DIR)
    if(NOT EXISTS "/usr/include/sol/sol.hpp" AND NOT EXISTS "/usr/local/include/sol/sol.hpp")
        message(FATAL_ERROR "Sol2 Header nicht gefunden!")
    endif()
endif()

include_directories(src/main/include)

set(REQPACK_PRIVATE_INCLUDE_DIRS
    ${SOL2_INCLUDE_DIR}
    src/main/include
)

if(LUA_INCLUDE_DIR AND NOT LUA_INCLUDE_DIR STREQUAL "/usr/include")
    list(APPEND REQPACK_PRIVATE_INCLUDE_DIRS ${LUA_INCLUDE_DIR})
endif()

set(REQPACK_CORE_REPO
    "https://github.com/Coditary/ReqPack-Core.git"
    CACHE STRING "Git repository for ReqPack-Core when not available locally"
)
set(REQPACK_CORE_TAG
    "main"
    CACHE STRING "Git tag, branch, or commit for ReqPack-Core FetchContent fallback"
)
set(REQPACK_CORE_DIR
    ""
    CACHE PATH "Optional local checkout of ReqPack-Core (skips FetchContent when valid)"
)

set(_reqpack_core_local "${CMAKE_CURRENT_SOURCE_DIR}/../ReqPack-Core")
if(REQPACK_CORE_DIR AND EXISTS "${REQPACK_CORE_DIR}/CMakeLists.txt")
    set(REQPACK_CORE_SOURCE_DIR "${REQPACK_CORE_DIR}")
    message(STATUS "Using local ReqPack-Core: ${REQPACK_CORE_SOURCE_DIR}")
elseif(EXISTS "${_reqpack_core_local}/CMakeLists.txt")
    set(REQPACK_CORE_SOURCE_DIR "${_reqpack_core_local}")
    message(STATUS "Using sibling ReqPack-Core: ${REQPACK_CORE_SOURCE_DIR}")
else()
    message(STATUS "Fetching ReqPack-Core from ${REQPACK_CORE_REPO} (${REQPACK_CORE_TAG})")
    FetchContent_Declare(
        reqpack_core
        GIT_REPOSITORY ${REQPACK_CORE_REPO}
        GIT_TAG ${REQPACK_CORE_TAG}
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(reqpack_core)
    set(REQPACK_CORE_SOURCE_DIR "${reqpack_core_SOURCE_DIR}")
endif()

set(REQPACK_CLI_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "ReqPack CLI repository path" FORCE)
set(REQPACK_CORE_BUILD_TESTS OFF CACHE BOOL "Build ReqPack-Core tests from the CLI repo" FORCE)
set(REQPACK_CORE_TEST_BUILD_DIR "${CMAKE_BINARY_DIR}" CACHE INTERNAL "Build dir used by core integration tests for the rqp binary")
if(SOL2_INCLUDE_DIR)
    set(SOL2_INCLUDE_DIR_OVERRIDE "${SOL2_INCLUDE_DIR}" CACHE INTERNAL "")
endif()

if(NOT TARGET reqpack_core)
    add_subdirectory(
        ${REQPACK_CORE_SOURCE_DIR}
        ${CMAKE_BINARY_DIR}/ReqPack-Core-build
    )
endif()

file(GLOB REQPACK_OUTPUT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/output/*.cpp"
)
add_library(reqpack_output STATIC ${REQPACK_OUTPUT_SOURCES})
target_include_directories(reqpack_output
    PUBLIC
        src/main/include
        ${REQPACK_CORE_SOURCE_DIR}/src/main/include
)
target_link_libraries(reqpack_output PUBLIC
    reqpack_security_core
    ${REQPACK_SPDLOG_TARGET}
    ${REQPACK_FMT_TARGET}
)
target_include_directories(reqpack_core PUBLIC src/main/include)
target_link_libraries(reqpack_core PUBLIC reqpack_output)

file(GLOB REQPACK_CLI_LIB_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/cli/*.cpp"
)
add_library(reqpack_cli STATIC ${REQPACK_CLI_LIB_SOURCES})
target_include_directories(reqpack_cli
    PUBLIC src/main/include
    PRIVATE src/main/cpp/cli
)
target_link_libraries(reqpack_cli PUBLIC
    reqpack_core
    reqpack_output
    CLI11::CLI11
)

set(REQPACK_CLI_SOURCES
    src/main/cpp/main.cpp
    src/main/cpp/main_arg_parsing.cpp
    src/main/cpp/main_diagnostics.cpp
    src/main/cpp/main_dispatch.cpp
    src/main/cpp/main_self_update.cpp
    src/main/cpp/main_self_update_support.cpp
    src/main/cpp/main_self_update_release.cpp
    src/main/cpp/main_stdin.cpp
)

add_executable(ReqPack ${REQPACK_CLI_SOURCES})
set_target_properties(ReqPack PROPERTIES OUTPUT_NAME rqp)

add_executable(reqpack_cli_unit_tests
    tests/cpp/test_main.cpp
    tests/unit/output/logger/format_tests.cpp
    tests/unit/output/run_result_json_tests.cpp
)

target_include_directories(reqpack_cli_unit_tests PRIVATE
    ${REQPACK_PRIVATE_INCLUDE_DIRS}
    tests/cpp
)

set(_reqpack_test_target_deps reqpack_cli_unit_tests)
if(RQP_SECURITY_CORE_BUILD_TESTS AND TARGET security_core_unit_tests)
    list(APPEND _reqpack_test_target_deps security_core_unit_tests)
endif()

add_custom_target(reqpack_test_targets
    DEPENDS ${_reqpack_test_target_deps}
)

add_dependencies(ReqPack reqpack_trivy_helper)

foreach(reqpack_target IN ITEMS ReqPack reqpack_cli_unit_tests reqpack_output reqpack_cli)
    reqpack_apply_instrumentation(${reqpack_target})
endforeach()

target_include_directories(ReqPack PRIVATE ${REQPACK_PRIVATE_INCLUDE_DIRS})
target_include_directories(reqpack_cli_unit_tests PRIVATE ${REQPACK_PRIVATE_INCLUDE_DIRS})

target_link_libraries(ReqPack PRIVATE
    reqpack_cli
    reqpack_core
    reqpack_output
    reqpack_security_core
    CLI11::CLI11
    CURL::libcurl
    lmdb
    lua_cffi
    OpenSSL::Crypto
    ${REQPACK_PLATFORM_LIBS}
    ${REQPACK_ZSTD_LIBRARY}
    ${REQPACK_LUA_LINK_LIBRARIES}
    Boost::boost
    ${REQPACK_SPDLOG_TARGET}
    ${REQPACK_FMT_TARGET}
)

target_link_libraries(reqpack_cli_unit_tests PRIVATE
    Catch2::Catch2
    reqpack_cli
    reqpack_core
    reqpack_output
    CLI11::CLI11
    ${REQPACK_SPDLOG_TARGET}
    ${REQPACK_FMT_TARGET}
)

target_compile_definitions(ReqPack PRIVATE SOL_ALL_SAFETIES_ON=1)
target_compile_definitions(ReqPack PRIVATE REQPACK_RELEASE_ID="${REQPACK_RELEASE_ID}")
target_compile_definitions(ReqPack PRIVATE REQPACK_TRIVY_HELPER_DEFAULT_PATH="${REQPACK_TRIVY_HELPER_PATH}")
target_compile_definitions(reqpack_cli_unit_tests PRIVATE
    SOL_ALL_SAFETIES_ON=1
    REQPACK_TEST_REPO_ROOT="${CMAKE_SOURCE_DIR}"
    REQPACK_TEST_BUILD_DIR="${CMAKE_BINARY_DIR}"
)

foreach(_reqpack_target IN ITEMS ReqPack reqpack_cli_unit_tests)
    reqpack_apply_windows_compile_options(${_reqpack_target})
endforeach()

include(CTest)
include(${catch2_SOURCE_DIR}/contrib/Catch.cmake)
catch_discover_tests(reqpack_cli_unit_tests TEST_PREFIX "cli::unit::")
