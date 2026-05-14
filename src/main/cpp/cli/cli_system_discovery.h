#pragma once

#include "core/config/configuration.h"

#include <set>
#include <string>

namespace cli_internal {

std::set<std::string> discover_primary_systems(const ReqPackConfig& config);
std::set<std::string> discover_non_builtin_plugins(const ReqPackConfig& config);
std::set<std::string> discover_systems(const ReqPackConfig& config);

}  // namespace cli_internal
