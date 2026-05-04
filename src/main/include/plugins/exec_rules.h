#pragma once

#include <sol/sol.hpp>

#include <string>

#include "output/logger.h"
#include "plugins/iplugin.h"

ExecResult run_plugin_command(Logger& logger,
	                          const std::string& sourceId,
	                          const std::string& pluginScope,
	                          const std::string& command,
	                          bool silent = false);
ExecResult run_plugin_command(Logger& logger,
	                          const std::string& sourceId,
	                          const std::string& pluginScope,
	                          const std::string& command,
	                          const sol::object& rules,
	                          bool silent = false);
