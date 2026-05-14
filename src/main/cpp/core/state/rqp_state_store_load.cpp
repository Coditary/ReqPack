#include "core/state/rqp_state_store.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>

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
