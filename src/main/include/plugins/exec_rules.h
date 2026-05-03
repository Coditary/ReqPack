#pragma once

#include <sol/sol.hpp>

#include <string>

#include "output/logger.h"
#include "plugins/iplugin.h"

ExecResult run_plugin_command(Logger& logger, const std::string& pluginId, const std::string& command, bool silent = false);
ExecResult run_plugin_command(Logger& logger, const std::string& pluginId, const std::string& command, const sol::object& rules, bool silent = false);
