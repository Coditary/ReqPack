#include "core/planner_core.h"

#include <algorithm>
#include <filesystem>

bool planner_same_package(const Package& left, const Package& right) {
    return left.action == right.action && left.system == right.system && left.name == right.name && left.version == right.version;
}

bool planner_contains_only_action(const std::vector<Request>& requests, ActionType action) {
    return !requests.empty() && std::all_of(requests.begin(), requests.end(), [action](const Request& request) {
        return request.action == action;
    });
}

std::string planner_package_specifier_from_package(const Package& package) {
    if (package.localTarget && !package.sourcePath.empty()) {
        return package.sourcePath;
    }
    if (package.version.empty()) {
        return package.name;
    }

    return package.name + "@" + package.version;
}

Package planner_normalize_dependency(Package dependency, const std::string& defaultSystem) {
    if (dependency.action == ActionType::UNKNOWN) {
        dependency.action = ActionType::INSTALL;
    }

    if (dependency.system.empty()) {
        dependency.system = defaultSystem;
    }

    return dependency;
}

std::vector<Request> planner_expand_proxies(
    const std::vector<Request>& requests,
    const std::map<std::string, std::string>& systemAliases
) {
    std::vector<Request> expandedRequests = requests;

    for (Request& request : expandedRequests) {
        auto alias = systemAliases.find(request.system);
        if (alias == systemAliases.end()) {
            continue;
        }

        request.system = alias->second;
    }

    return expandedRequests;
}

Package planner_make_requested_package(
    const Request& request,
    const std::string& resolvedSystem,
    const std::string& packageSpecifier
) {
    Package package;
    package.action = request.action;
    package.system = resolvedSystem;
    package.flags = request.flags;

    const std::size_t versionSeparator = packageSpecifier.rfind('@');
    if (versionSeparator == std::string::npos || versionSeparator == 0 || versionSeparator == packageSpecifier.size() - 1) {
        package.name = packageSpecifier;
        return package;
    }

    package.name = packageSpecifier.substr(0, versionSeparator);
    package.version = packageSpecifier.substr(versionSeparator + 1);
    return package;
}

Package planner_make_local_requested_package(
    const Request& request,
    const std::string& resolvedSystem
) {
    Package package;
    package.action = request.action;
    package.system = resolvedSystem;
    package.name = std::filesystem::path(request.localPath).filename().string();
    package.sourcePath = request.localPath;
    package.localTarget = true;
    package.flags = request.flags;
    return package;
}

Request planner_filter_request_to_missing_packages(
    const Request& request,
    const std::vector<Package>& missingPackages
) {
    Request filteredRequest = request;
    filteredRequest.packages.clear();
    filteredRequest.packages.reserve(missingPackages.size());
    for (const Package& missingPackage : missingPackages) {
        filteredRequest.packages.push_back(planner_package_specifier_from_package(missingPackage));
    }

    return filteredRequest;
}

std::optional<Request> planner_filter_install_request(
    const Request& request,
    const std::vector<Package>& missingPackages
) {
    if (missingPackages.empty()) {
        return std::nullopt;
    }

    return planner_filter_request_to_missing_packages(request, missingPackages);
}
