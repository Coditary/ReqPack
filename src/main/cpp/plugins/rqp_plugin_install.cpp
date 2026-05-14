#include "rqp_plugin_internal.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

bool RqpPlugin::install(const PluginCallContext& context, const std::vector<Package>& packages) {
    recentEvents_.clear();
    const std::vector<RqRepositoryIndex> indexes = loadRepositoryIndexes(context);
    if (indexes.empty()) {
        context.emitFailure("rqp repositories not configured");
        return false;
    }

    bool allInstalled = true;
    for (const Package& package : packages) {
        const std::optional<RqRepositoryPackage> resolvedPackage = rq_repository_resolve_package(
            indexes,
            package.name,
            package.version,
            rq_host_architecture(),
            rq_host_system_tokens(*HostInfoService::currentSnapshot()),
            config_
        );
        if (!resolvedPackage.has_value()) {
            recentEvents_.push_back(PluginEventRecord{.name = "unavailable", .payload = package.version.empty() ? package.name : package.name + "@" + package.version});
            context.emitFailure("package not found in rqp repositories: " + package.name);
            allInstalled = false;
            continue;
        }

        if (!installResolvedPackage(context, package, resolvedPackage.value())) {
            allInstalled = false;
        }
    }

    return allInstalled;
}

bool RqpPlugin::installLocal(const PluginCallContext& context, const std::string& path) {
    recentEvents_.clear();
    std::filesystem::path sourcePath(path);
    std::error_code error;
    if (std::filesystem::is_directory(sourcePath, error) && !error) {
        if (!std::filesystem::exists(sourcePath / "reqpack.lua", error) || error) {
            const std::optional<std::filesystem::path> nestedPackage = rqp_plugin_unique_nested_file_with_extension(sourcePath, ".rqp");
            if (!nestedPackage.has_value()) {
                context.emitFailure("no installable rqp file found in extracted archive");
                return false;
            }
            sourcePath = nestedPackage.value();
        }
    }
    return installPackagePath(context, sourcePath, "local-file", path);
}

bool RqpPlugin::remove(const PluginCallContext& context, const std::vector<Package>& packages) {
    recentEvents_.clear();
    RqpStateStore stateStore(config_);
    bool allRemoved = true;
    for (const Package& package : packages) {
        std::vector<RqpInstalledPackage> matches = stateStore.findInstalled(package.name, package.version);
        if (matches.empty()) {
            continue;
        }
        if (package.version.empty() && matches.size() > 1) {
            context.emitFailure("multiple installed versions found for package: " + package.name);
            allRemoved = false;
            continue;
        }
        std::sort(matches.begin(), matches.end(), [](const RqpInstalledPackage& left, const RqpInstalledPackage& right) {
            return compareInstalledVersions(left, right) > 0;
        });
        if (!removeInstalledPackage(context, matches.front())) {
            allRemoved = false;
        }
    }
    return allRemoved;
}

bool RqpPlugin::update(const PluginCallContext& context, const std::vector<Package>& packages) {
    recentEvents_.clear();
    RqpStateStore stateStore(config_);
    bool allUpdated = true;
    for (const Package& package : packages) {
        std::vector<RqpInstalledPackage> matches = stateStore.findInstalled(package.name, package.version);
        if (matches.empty()) {
            continue;
        }
        if (package.version.empty() && matches.size() > 1) {
            context.emitFailure("multiple installed versions found for package: " + package.name);
            allUpdated = false;
            continue;
        }

        const RqpInstalledPackage& installed = matches.front();
        if (installed.source.source != "repository") {
            continue;
        }
        if (config_.rqp.repositories.empty()) {
            continue;
        }

        const std::vector<RqRepositoryIndex> indexes = loadRepositoryIndexes(context);
        const std::optional<RqRepositoryPackage> candidate = rq_repository_resolve_package(
            indexes,
            installed.metadata.name,
            {},
            rq_host_architecture(),
            rq_host_system_tokens(*HostInfoService::currentSnapshot()),
            config_
        );
        if (!candidate.has_value() || !repositoryCandidateIsNewer(installed, candidate.value())) {
            continue;
        }
        if (!removeInstalledPackage(context, installed)) {
            allUpdated = false;
            continue;
        }
        Package requested;
        requested.name = installed.metadata.name;
        requested.version = installed.metadata.version;
        if (!installResolvedPackage(context, requested, candidate.value())) {
            allUpdated = false;
        }
    }
    return allUpdated;
}

bool RqpPlugin::installPackagePath(
    const PluginCallContext& context,
    const std::filesystem::path& path,
    const std::string& sourceType,
    const std::string& sourceValue,
    const std::string& repository
) {
    try {
        pendingManifest_.clear();
        context.emitBeginStep("load rqp package");
        const std::filesystem::path stateRoot = std::filesystem::path(config_.rqp.statePath);
        const std::filesystem::path workRoot = std::filesystem::temp_directory_path() / "reqpack-rqp";
        const RqPackageLayout layout = RqPackageReader::load(path, workRoot, stateRoot, config_);

        context.emitBeginStep("run install hook");
        if (!runHook(context, layout, "install")) {
            return false;
        }

        context.emitBeginStep("persist rqp state");
        if (!persistInstalledState(layout, sourceType, sourceValue, repository)) {
            context.emitFailure("failed to persist rqp state");
            return false;
        }

        context.emitSuccess();
        return true;
    } catch (const std::exception& error) {
        context.emitFailure(error.what());
        return false;
    }
}

bool RqpPlugin::installResolvedPackage(const PluginCallContext& context, const Package& package, const RqRepositoryPackage& resolvedPackage) {
    context.emitBeginStep("resolve rqp repository package");
    const std::filesystem::path localArtifactPath = downloadPackageArtifact(context, resolvedPackage.url);
    if (localArtifactPath.empty()) {
        context.emitFailure("failed to download rqp package artifact");
        return false;
    }

    if (!resolvedPackage.packageSha256.empty()) {
        context.emitBeginStep("verify repository artifact sha256");
        const std::string actualSha256 = sha256Hex(readTextFile(localArtifactPath));
        if (actualSha256 != resolvedPackage.packageSha256) {
            context.emitFailure("repository package sha256 mismatch");
            return false;
        }
    }

    const std::string sourceValue = resolvedPackage.name + "@" + resolvedPackage.version;
    return installPackagePath(context, localArtifactPath, "repository", sourceValue, resolvedPackage.repository);
}

bool RqpPlugin::removeInstalledPackage(const PluginCallContext& context, const RqpInstalledPackage& installed) {
    context.emitBeginStep("load installed rqp state");
    if (!runInstalledHook(context, installed, "remove")) {
        return false;
    }
    context.emitBeginStep("remove manifest artifacts");
    if (!removeManifestArtifacts(installed)) {
        context.emitFailure("failed to remove manifest artifacts");
        return false;
    }
    context.emitBeginStep("delete rqp state");
    if (!RqpStateStore(config_).removeInstalledState(installed)) {
        context.emitFailure("failed to delete rqp state");
        return false;
    }
    context.emitSuccess();
    return true;
}
