cmake_minimum_required(VERSION 3.15)
project(ReqPack LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Lua REQUIRED)
find_package(CLI11 REQUIRED)
find_package(Boost REQUIRED)

find_path(SOL2_INCLUDE_DIR 
    NAMES sol/sol.hpp
    PATHS /usr/include /usr/local/include
)

if(NOT SOL2_INCLUDE_DIR)
    message(FATAL_ERROR "Sol2 Header nicht gefunden!")
endif()

if(Boost_FOUND)
    message(STATUS "Boost gefunden (Version: ${Boost_VERSION})")
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
    src/main/cpp/plugins/bridge.cpp
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
	${LUA_LIBRARIES}
	Boost::boost
)

target_compile_definitions(ReqPack PRIVATE SOL_ALL_SAFETIES_ON=1)
