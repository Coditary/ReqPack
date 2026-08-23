#include <catch2/catch.hpp>

#include <sol/sol.hpp>

#include "core/luaenv/lua_env_ffi.h"

namespace {

bool is_table(const sol::state& lua, const char* name) {
    const sol::object value = const_cast<sol::state&>(lua)[name];
    return value.valid() && value.get_type() == sol::type::table;
}

}  // namespace

TEST_CASE("lua env registers ffi as global and requireable module", "[unit][luaenv][ffi]") {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::package);

    ensure_ffi_available(lua);

    CHECK(is_table(lua, "ffi"));

    const sol::object loaded = lua.script("return package.loaded['ffi']");
    REQUIRE(loaded.valid());
    CHECK(loaded.get_type() == sol::type::table);

    const sol::object required = lua.script("return require('ffi')");
    REQUIRE(required.valid());
    CHECK(required.get_type() == sol::type::table);
}

TEST_CASE("lua env ffi supports cdef and sizeof", "[unit][luaenv][ffi]") {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::package);

    ensure_ffi_available(lua);

    lua.script(R"(
        ffi.cdef([[
            typedef struct { int a; double b; } reqpack_ffi_probe_t;
        ]])
    )");

    const int intSize = lua.script("return ffi.sizeof('int')");
    CHECK(intSize == 4);

    const int structSize = lua.script("return ffi.sizeof('reqpack_ffi_probe_t')");
    CHECK(structSize == 16);
}

TEST_CASE("lua env ffi registration is idempotent", "[unit][luaenv][ffi]") {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::package);

    ensure_ffi_available(lua);
    lua.script("reqpack_ffi_before = package.loaded['ffi']");

    ensure_ffi_available(lua);

    const bool sameModule = lua.script(
        "return reqpack_ffi_before ~= nil and reqpack_ffi_before == package.loaded['ffi']");
    CHECK(sameModule);
}

TEST_CASE("lua env ffi works without package library open", "[unit][luaenv][ffi]") {
    sol::state lua;
    lua.open_libraries(sol::lib::base);

    ensure_ffi_available(lua);

    CHECK(is_table(lua, "ffi"));
    const int size = lua.script("return ffi.sizeof('double')");
    CHECK(size == 8);
}
