#pragma once

#include "plugins/lua_bridge_host_runtime.h"
#include "plugins/lua_bridge_runtime.h"

class LuaBridgeBindings {
public:
    LuaBridgeBindings(LuaBridgeScriptRuntime& runtime, LuaBridgeHostRuntime& hostRuntime);

    void registerBuiltinTypes();
    void registerContextTypes();
    void registerReqpackNamespace();

private:
    LuaBridgeScriptRuntime& m_runtime;
    LuaBridgeHostRuntime& m_hostRuntime;
};
