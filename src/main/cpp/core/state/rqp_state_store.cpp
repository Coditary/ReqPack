#include "core/state/rqp_state_store.h"

#include <algorithm>
#include <system_error>

RqpStateStore::RqpStateStore(const ReqPackConfig& config) : config_(config) {}

std::vector<RqpInstalledPackage> RqpStateStore::listInstalled() const {
    std::vector<RqpInstalledPackage> installed;
    const std::filesystem::path root(config_.rqp.statePath);
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) {
        return installed;
    }

    for (const auto& packageEntry : std::filesystem::directory_iterator(root, error)) {
        if (error || !packageEntry.is_directory()) {
            continue;
        }
        for (const auto& identityEntry : std::filesystem::directory_iterator(packageEntry.path(), error)) {
            if (error || !identityEntry.is_directory()) {
                continue;
            }
            if (const auto loaded = loadInstalled(identityEntry.path())) {
                installed.push_back(loaded.value());
            }
        }
    }

    std::sort(installed.begin(), installed.end(), [](const RqpInstalledPackage& left, const RqpInstalledPackage& right) {
        if (left.metadata.name != right.metadata.name) {
            return left.metadata.name < right.metadata.name;
        }
        return left.identity < right.identity;
    });
    return installed;
}

std::vector<RqpInstalledPackage> RqpStateStore::findInstalled(const std::string& name, const std::string& version) const {
    std::vector<RqpInstalledPackage> matches;
    for (const RqpInstalledPackage& installed : listInstalled()) {
        if (installed.metadata.name != name) {
            continue;
        }
        if (!version.empty() && installed.metadata.version != version) {
            continue;
        }
        matches.push_back(installed);
    }
    return matches;
}

bool RqpStateStore::removeInstalledState(const RqpInstalledPackage& installed) const {
    std::error_code error;
    std::filesystem::remove_all(installed.stateDir, error);
    if (error) {
        return false;
    }

    const std::filesystem::path packageRoot = installed.stateDir.parent_path();
    if (packageRoot.empty()) {
        return true;
    }
    if (std::filesystem::is_empty(packageRoot, error) && !error) {
        std::filesystem::remove(packageRoot, error);
    }
    return !error;
}
