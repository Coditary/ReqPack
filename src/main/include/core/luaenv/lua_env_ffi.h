#pragma once

#include <sol/sol.hpp>

/// Guarantees that an embedded Lua environment exposes the `ffi` module,
/// independent of the linked Lua implementation.
///
/// - On LuaJIT builds the built-in `ffi` table already exists and is left untouched.
/// - On standard Lua 5.x builds the vendored cffi-lua module (q66/cffi-lua,
///   statically linked into ReqPack on top of libffi) is registered instead:
///   as global `ffi`, inside `package.loaded["ffi"]`, and therefore also
///   resolvable through `require("ffi")`.
void ensure_ffi_available(sol::state& lua);
