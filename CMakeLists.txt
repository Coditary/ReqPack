cmake_minimum_required(VERSION 3.15)
project(ReqPack LANGUAGES CXX)
enable_language(C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

find_package(Lua REQUIRED)
find_package(CLI11 REQUIRED)
find_package(CURL REQUIRED)
find_package(Boost REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

FetchContent_Declare(
    lmdb
    GIT_REPOSITORY https://github.com/LMDB/lmdb.git
    GIT_TAG LMDB_0.9.33
)
FetchContent_MakeAvailable(lmdb)

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
    src/main/cpp/plugins/lua_bridge.cpp
    src/main/cpp/core/configuration.cpp
    src/main/cpp/core/downloader.cpp
    src/main/cpp/core/registry_database.cpp
    src/main/cpp/core/orchestrator.cpp
    src/main/cpp/core/planner.cpp
    src/main/cpp/core/validator.cpp
    src/main/cpp/core/executor.cpp
    src/main/cpp/core/registry.cpp
)

add_executable(ReqPack ${SOURCES})

target_include_directories(ReqPack PRIVATE 
    ${SOL2_INCLUDE_DIR}
    ${LUA_INCLUDE_DIR}
    src/main/include
)

target_link_libraries(ReqPack PRIVATE 
	CLI11::CLI11
	CURL::libcurl
	lmdb
	${LUA_LIBRARIES}
	Boost::boost
	spdlog::spdlog
	fmt::fmt
)

target_compile_definitions(ReqPack PRIVATE SOL_ALL_SAFETIES_ON=1)
