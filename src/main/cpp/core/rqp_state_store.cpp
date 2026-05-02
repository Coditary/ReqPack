#include "core/rqp_state_store.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <sstream>
#include <stdexcept>
#include <system_error>

namespace {

using boost::property_tree::ptree;

std::optional<ptree> parse_json_tree(const std::string& json) {
    if (json.empty()) {
        return std::nullopt;
    }

    std::istringstream input(json);
    ptree tree;
    try {
        boost::property_tree::read_json(input, tree);
        return tree;
    } catch (...) {
        return std::nullopt;
    }
}

std::string required_string(const ptree& tree, const std::string& key) {
    const auto value = tree.get_optional<std::string>(key);
    if (!value.has_value() || value->empty()) {
        throw std::runtime_error("state source missing string field: " + key);
    }
    return value.value();
}

}  // namespace

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

std::optional<RqpInstalledPackage> RqpStateStore::loadInstalled(const std::filesystem::path& stateDir) const {
    const std::filesystem::path metadataPath = stateDir / "metadata.json";
    const std::filesystem::path reqpackLuaPath = stateDir / "reqpack.lua";
    const std::filesystem::path sourcePath = stateDir / "source.json";
    const std::filesystem::path manifestPath = stateDir / "manifest.json";

    if (!std::filesystem::is_regular_file(metadataPath) || !std::filesystem::is_regular_file(reqpackLuaPath) ||
        !std::filesystem::is_regular_file(sourcePath) || !std::filesystem::is_regular_file(manifestPath)) {
        return std::nullopt;
    }

    RqpInstalledPackage installed;
    installed.metadata = rq_parse_metadata_json(readTextFile(metadataPath));
    installed.source = parseSourceJson(readTextFile(sourcePath));
    installed.hooks = rq_parse_reqpack_hooks(reqpackLuaPath);
    installed.identity = rq_package_identity(installed.metadata);
    installed.stateDir = stateDir;
    installed.metadataPath = metadataPath;
    installed.reqpackLuaPath = reqpackLuaPath;
    installed.scriptsDir = stateDir / "scripts";
    installed.manifestPath = manifestPath;
    installed.sourcePath = sourcePath;
    return installed;
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

RqStateSource RqpStateStore::parseSourceJson(const std::string& content) {
    const std::optional<ptree> parsed = parse_json_tree(content);
    if (!parsed.has_value()) {
        throw std::runtime_error("invalid source.json");
    }

    const ptree& tree = parsed.value();
    return RqStateSource{
        .source = required_string(tree, "source"),
        .path = required_string(tree, "path"),
        .repository = tree.get<std::string>("repository", {}),
        .identity = required_string(tree, "identity"),
    };
}

std::string RqpStateStore::readTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("failed to open file: " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}
