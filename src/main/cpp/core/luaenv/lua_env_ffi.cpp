#include "core/luaenv/lua_env_ffi.h"

// Vendored cffi-lua entry point (q66/cffi-lua, linked statically as lua_cffi).
extern "C" int luaopen_cffi(lua_State* L);

namespace {

bool ffi_table_present(sol::state& lua) {
    const sol::object existing = lua["ffi"];
    return existing.valid() && existing.get_type() == sol::type::table;
}

}  // namespace

void ensure_ffi_available(sol::state& lua) {
    // LuaJIT ships its own ffi module; never shadow it with the vendored one.
    if (ffi_table_present(lua)) {
        return;
    }

    luaL_requiref(lua.lua_state(), "ffi", luaopen_cffi, 1);
    lua_pop(lua.lua_state(), 1);
}
