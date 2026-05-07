#pragma once

#include "core/common/types.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

bool planner_same_package(const Package& left, const Package& right);
bool planner_contains_only_action(const std::vector<Request>& requests, ActionType action);
std::string planner_package_specifier_from_package(const Package& package);
Package planner_normalize_dependency(Package dependency, const std::string& defaultSystem);
std::vector<Request> planner_expand_proxies(
    const std::vector<Request>& requests,
    const std::map<std::string, std::string>& systemAliases
);
Package planner_make_requested_package(
    const Request& request,
    const std::string& resolvedSystem,
    const std::string& packageSpecifier
);
Package planner_make_local_requested_package(
    const Request& request,
    const std::string& resolvedSystem
);
Request planner_filter_request_to_missing_packages(
    const Request& request,
    const std::vector<Package>& missingPackages
);
std::optional<Request> planner_filter_install_request(
    const Request& request,
    const std::vector<Package>& missingPackages
);
