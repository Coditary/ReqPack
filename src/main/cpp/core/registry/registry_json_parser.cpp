#include "core/registry/registry_json_parser.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace {

using boost::property_tree::ptree;

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("failed to open registry json: " + path.string());
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

ptree parse_json_tree(const std::string& json, const std::filesystem::path& path) {
    std::istringstream input(json);
    ptree tree;
    try {
        boost::property_tree::read_json(input, tree);
    } catch (const std::exception& error) {
        throw std::runtime_error("invalid registry json '" + path.string() + "': " + error.what());
    }
    return tree;
}

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string trim_copy(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string required_string(const ptree& tree, const std::string& key, const std::filesystem::path& path) {
    const auto value = tree.get_optional<std::string>(key);
    if (!value.has_value()) {
        throw std::runtime_error("registry json missing string field '" + key + "' in '" + path.string() + "'");
    }

    const std::string trimmed = trim_copy(value.value());
    if (trimmed.empty()) {
        throw std::runtime_error("registry json field '" + key + "' is empty in '" + path.string() + "'");
    }
    return trimmed;
}

std::vector<std::string> load_string_array(const boost::optional<const ptree&>& values, bool normalizeLower = false) {
    std::vector<std::string> result;
    if (!values.has_value()) {
        return result;
    }

    for (const auto& [_, child] : values.value()) {
        const std::string raw = trim_copy(child.get_value<std::string>());
        if (raw.empty()) {
            continue;
        }
        result.push_back(normalizeLower ? to_lower_copy(raw) : raw);
    }

    return result;
}

std::vector<RegistryWriteScope> load_write_scopes(const boost::optional<const ptree&>& values) {
    std::vector<RegistryWriteScope> scopes;
    if (!values.has_value()) {
        return scopes;
    }

    for (const auto& [_, child] : values.value()) {
        const auto kind = child.get_optional<std::string>("kind");
        if (!kind.has_value()) {
            continue;
        }

        RegistryWriteScope scope;
        scope.kind = to_lower_copy(trim_copy(kind.value()));
        scope.value = trim_copy(child.get<std::string>("value", {}));
        if (!scope.kind.empty()) {
            scopes.push_back(std::move(scope));
        }
    }

    return scopes;
}

std::vector<RegistryNetworkScope> load_network_scopes(const boost::optional<const ptree&>& values) {
    std::vector<RegistryNetworkScope> scopes;
    if (!values.has_value()) {
        return scopes;
    }

    for (const auto& [_, child] : values.value()) {
        RegistryNetworkScope scope;
        scope.host = to_lower_copy(trim_copy(child.get<std::string>("host", {})));
        scope.scheme = to_lower_copy(trim_copy(child.get<std::string>("scheme", {})));
        scope.pathPrefix = trim_copy(child.get<std::string>("pathPrefix", {}));
        if (scope.host.empty() && scope.scheme.empty() && scope.pathPrefix.empty()) {
            continue;
        }
        scopes.push_back(std::move(scope));
    }

    return scopes;
}

std::string path_filename_stem_lower(const std::filesystem::path& path) {
    return to_lower_copy(path.stem().string());
}

RegistryRecord build_main_record(const ptree& tree, const std::filesystem::path& path, const std::string& originPath) {
    RegistryRecord record;
    record.name = to_lower_copy(required_string(tree, "name", path));
    record.source = required_string(tree, "source", path);
    record.alias = false;
    record.description = trim_copy(tree.get<std::string>("description", {}));
    record.role = to_lower_copy(trim_copy(tree.get<std::string>("role", {})));
    record.capabilities = load_string_array(tree.get_child_optional("capabilities"), true);
    record.ecosystemScopes = load_string_array(tree.get_child_optional("ecosystemScopes"), true);
    record.writeScopes = load_write_scopes(tree.get_child_optional("writeScopes"));
    record.networkScopes = load_network_scopes(tree.get_child_optional("networkScopes"));
    record.privilegeLevel = to_lower_copy(trim_copy(tree.get<std::string>("privilegeLevel", {})));
    record.scriptSha256 = to_lower_copy(trim_copy(tree.get<std::string>("scriptSha256", {})));
    record.bootstrapSha256 = to_lower_copy(trim_copy(tree.get<std::string>("bootstrapSha256", {})));
    record.originPath = originPath;
    return record;
}

std::vector<RegistryRecord> build_alias_records(
    const ptree& tree,
    const RegistryRecord& mainRecord,
    const std::filesystem::path& path,
    const std::string& originPath
) {
    std::vector<RegistryRecord> aliases;
    const auto aliasNodes = tree.get_child_optional("aliases");
    if (!aliasNodes.has_value()) {
        return aliases;
    }

    for (const auto& [_, child] : aliasNodes.value()) {
        RegistryRecord aliasRecord;
        aliasRecord.alias = true;
        aliasRecord.source = mainRecord.name;
        aliasRecord.originPath = originPath;

        const auto nameNode = child.get_optional<std::string>("name");
        if (nameNode.has_value()) {
            aliasRecord.name = to_lower_copy(trim_copy(nameNode.value()));
            aliasRecord.description = trim_copy(child.get<std::string>("description", {}));
        } else {
            aliasRecord.name = to_lower_copy(trim_copy(child.get_value<std::string>()));
        }

        if (aliasRecord.name.empty()) {
            throw std::runtime_error("registry json alias missing name in '" + path.string() + "'");
        }

        aliases.push_back(std::move(aliasRecord));
    }

    return aliases;
}

}  // namespace

RegistryJsonParseResult parse_registry_json_file(const std::filesystem::path& path) {
    if (path.extension() != ".json") {
        throw std::runtime_error("registry file must be json: " + path.string());
    }

    const ptree tree = parse_json_tree(read_text_file(path), path);
    if (tree.get<int>("schemaVersion", 0) != 1) {
        throw std::runtime_error("unsupported registry schemaVersion in '" + path.string() + "'");
    }

    const std::string expectedName = path_filename_stem_lower(path);
    const std::string originPath = path.generic_string();
    RegistryRecord mainRecord = build_main_record(tree, path, originPath);
    if (mainRecord.name != expectedName) {
        throw std::runtime_error("registry json name must match filename in '" + path.string() + "'");
    }

    RegistryJsonParseResult result;
    result.records.push_back(mainRecord);
    std::vector<RegistryRecord> aliases = build_alias_records(tree, mainRecord, path, originPath);
    result.records.insert(result.records.end(), aliases.begin(), aliases.end());
    return result;
}
