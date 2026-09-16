#pragma once

#include "core/config/configuration.h"

#include <set>
#include <string>
#include <vector>

namespace cli_internal {

std::set<std::string> discover_primary_systems(const ReqPackConfig& config);
std::vector<std::string> discover_installed_plugins(const ReqPackConfig& config);
std::set<std::string> discover_systems(const ReqPackConfig& config);

}  // namespace cli_internal
