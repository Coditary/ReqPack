#include "cli_parse_targets.h"

#include "cli_parse_shared.h"

#include <reqpack/manifest_loader.h>
#include "output/diagnostic.h"
#include "output/logger.h"

#include <unordered_map>

namespace {

DiagnosticMessage manifest_missing_diagnostic(const std::filesystem::path& manifestPath) {
    return make_error_diagnostic(
        "cli",
        "Manifest not found: " + MANIFEST_FILENAME + " missing in '" + manifestPath.parent_path().string() + "'",
        "Command was given a directory path, but that directory does not contain a reqpack.lua manifest.",
        "Create reqpack.lua in that directory or pass packages directly instead of a manifest path.",
        {},
        "cli",
        "manifest",
        {{"path", manifestPath.parent_path().string()}}
    );
}

DiagnosticMessage manifest_load_diagnostic(const std::filesystem::path& manifestPath, const std::string& details) {
    return make_error_diagnostic(
        "cli",
        "Manifest load failed: '" + manifestPath.string() + "'",
        "Manifest file exists but could not be parsed or executed.",
        "Check Lua syntax and manifest structure, then run command again.",
        details,
        "cli",
        "manifest",
        {{"path", manifestPath.string()}}
    );
}

DiagnosticMessage manifest_empty_diagnostic(const std::filesystem::path& manifestPath) {
    return make_error_diagnostic(
        "cli",
        "Manifest contains no packages: '" + manifestPath.string() + "'",
        "reqpack.lua loaded successfully but returned no installable package entries.",
        "Add entries under packages = { ... } or use direct package arguments.",
        {},
        "cli",
        "manifest",
        {{"path", manifestPath.string()}}
    );
}

}  // namespace

namespace cli_internal {

bool uses_unified_package_targets(ActionType action) {
    switch (action) {
        case ActionType::INSTALL:
        case ActionType::REMOVE:
        case ActionType::UPDATE:
        case ActionType::LIST:
        case ActionType::OUTDATED:
        case ActionType::INFO:
        case ActionType::AUDIT:
        case ActionType::SBOM:
            return true;
        default:
            return false;
    }
}

std::optional<std::vector<Request>> build_requests_from_manifest_argument(
    ActionType action,
    const std::string& argument,
    const std::vector<std::string>& globalFlags,
    const std::function<void(Request&)>& assignOutputOptions,
    CliParseState& state
) {
    if (!uses_unified_package_targets(action)) {
        return std::nullopt;
    }

    const std::optional<std::filesystem::path> manifestPath = resolve_manifest_path_argument(argument);
    if (!manifestPath.has_value()) {
        return std::nullopt;
    }

    if (!std::filesystem::exists(manifestPath.value())) {
        Logger::instance().diagnostic(manifest_missing_diagnostic(manifestPath.value()));
        state.lastParseFailed = true;
        return std::vector<Request> {};
    }

    std::vector<ManifestEntry> entries;
    try {
        entries = ManifestLoader::load(manifestPath.value());
    } catch (const std::exception& error) {
        Logger::instance().diagnostic(manifest_load_diagnostic(manifestPath.value(), error.what()));
        state.lastParseFailed = true;
        return std::vector<Request> {};
    }

    if (entries.empty()) {
        Logger::instance().diagnostic(manifest_empty_diagnostic(manifestPath.value()));
        state.lastParseFailed = true;
        return std::vector<Request> {};
    }

    std::vector<Request> requests;
    std::unordered_map<std::string, std::size_t> manifestIndex;
    for (const ManifestEntry& entry : entries) {
        const std::string normalized = to_lower_copy(entry.system);
        const auto [it, inserted] = manifestIndex.emplace(normalized, requests.size());
        if (inserted) {
            requests.push_back(Request {.action = action, .system = normalized});
        }
        Request& request = requests[it->second];
        const std::string packageSpecifier =
            entry.version.empty() ? entry.name : (entry.name + "@" + entry.version);
        request.packages.push_back(packageSpecifier);
        for (const std::string& flag : entry.flags) {
            request.flags.push_back(flag);
        }
    }

    for (Request& request : requests) {
        for (const std::string& flag : globalFlags) {
            request.flags.push_back(flag);
        }
        assignOutputOptions(request);
    }

    return requests;
}

}  // namespace cli_internal
