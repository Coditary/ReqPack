#include "rq_package_internal.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <sol/sol.hpp>

#include <optional>
#include <sstream>
#include <stdexcept>

namespace {

using boost::property_tree::ptree;

std::string json_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped.push_back(ch); break;
        }
    }
    return escaped;
}

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

std::vector<std::string> parse_string_or_array_field(const ptree& tree, const std::string& key) {
    if (const auto node = tree.get_child_optional(key)) {
        if (node->empty()) {
            if (const auto stringValue = tree.get_optional<std::string>(key)) {
                return {stringValue.value()};
            }
            return {};
        }

        std::vector<std::string> values;
        for (const auto& [childKey, child] : node.value()) {
            if (!childKey.empty()) {
                throw std::runtime_error("metadata field must be string or array of strings: " + key);
            }
            values.push_back(child.get_value<std::string>());
        }
        return values;
    }
    return {};
}

int required_int(const ptree& tree, const std::string& key) {
    const auto value = tree.get_optional<int>(key);
    if (!value.has_value()) {
        throw std::runtime_error("metadata missing integer field: " + key);
    }
    return value.value();
}

std::string required_string(const ptree& tree, const std::string& key) {
    const auto value = tree.get_optional<std::string>(key);
    if (!value.has_value() || value->empty()) {
        throw std::runtime_error("metadata missing string field: " + key);
    }
    return value.value();
}

std::vector<std::string> load_string_array(const boost::optional<const ptree&>& values) {
    std::vector<std::string> result;
    if (!values.has_value()) {
        return result;
    }
    for (const auto& [_, child] : values.value()) {
        result.push_back(child.get_value<std::string>());
    }
    return result;
}

std::vector<RqBinaryEntry> load_binaries(const boost::optional<const ptree&>& values) {
    std::vector<RqBinaryEntry> result;
    if (!values.has_value()) {
        return result;
    }
    for (const auto& [_, child] : values.value()) {
        result.push_back(RqBinaryEntry{
            .name = child.get<std::string>("name", {}),
            .installPath = child.get<std::string>("installPath", {}),
            .primary = child.get<bool>("primary", false),
        });
    }
    return result;
}

}  // namespace

namespace rq_package_internal {

RqMetadata parse_metadata_json_impl(const std::string& content) {
    const std::optional<ptree> parsed = parse_json_tree(content);
    if (!parsed.has_value()) {
        throw std::runtime_error("invalid metadata json");
    }

    const ptree& tree = parsed.value();
    RqMetadata metadata;
    metadata.formatVersion = required_int(tree, "formatVersion");
    metadata.name = required_string(tree, "name");
    metadata.version = required_string(tree, "version");
    metadata.release = required_int(tree, "release");
    metadata.revision = required_int(tree, "revision");
    metadata.summary = required_string(tree, "summary");
    metadata.description = required_string(tree, "description");
    metadata.license = required_string(tree, "license");
    metadata.architecture = rq_normalize_architecture(tree.get<std::string>("architecture", {}));
    metadata.systems = rq_normalize_systems(parse_string_or_array_field(tree, "system"));
    metadata.vendor = required_string(tree, "vendor");
    metadata.maintainerEmail = required_string(tree, "maintainerEmail");
    metadata.tags = load_string_array(tree.get_child_optional("tags"));
    metadata.url = required_string(tree, "url");
    metadata.homepage = tree.get<std::string>("homepage", {});
    metadata.sourceUrl = tree.get<std::string>("sourceUrl", {});
    metadata.packager = tree.get<std::string>("packager", {});
    metadata.buildDate = tree.get<std::string>("buildDate", {});
    metadata.binaries = load_binaries(tree.get_child_optional("binaries"));
    metadata.depends = load_string_array(tree.get_child_optional("depends"));
    metadata.provides = load_string_array(tree.get_child_optional("provides"));
    metadata.conflicts = load_string_array(tree.get_child_optional("conflicts"));
    metadata.replaces = load_string_array(tree.get_child_optional("replaces"));

    if (metadata.formatVersion != 1) {
        throw std::runtime_error("unsupported rqp format version");
    }

    if (const auto payloadNode = tree.get_child_optional("payload")) {
        RqPayloadMetadata payload;
        payload.path = required_string(payloadNode.value(), "path");
        payload.archive = required_string(payloadNode.value(), "archive");
        payload.compression = required_string(payloadNode.value(), "compression");
        payload.hashAlgorithm = required_string(payloadNode.value(), "hashAlgorithm");
        payload.hashFile = required_string(payloadNode.value(), "hashFile");
        payload.sizeCompressed = payloadNode->get<std::uint64_t>("sizeCompressed", 0);
        payload.sizeInstalledExpected = payloadNode->get<std::uint64_t>("sizeInstalledExpected", 0);
        metadata.payload = payload;
    }

    return metadata;
}

std::string metadata_json_impl(const RqMetadata& metadata) {
    std::ostringstream stream;
    stream << "{\n";
    stream << "  \"formatVersion\": " << metadata.formatVersion << ",\n";
    stream << "  \"name\": \"" << json_escape(metadata.name) << "\",\n";
    stream << "  \"version\": \"" << json_escape(metadata.version) << "\",\n";
    stream << "  \"release\": " << metadata.release << ",\n";
    stream << "  \"revision\": " << metadata.revision << ",\n";
    stream << "  \"summary\": \"" << json_escape(metadata.summary) << "\",\n";
    stream << "  \"description\": \"" << json_escape(metadata.description) << "\",\n";
    stream << "  \"license\": \"" << json_escape(metadata.license) << "\",\n";
    stream << "  \"architecture\": \"" << json_escape(rq_normalize_architecture(metadata.architecture)) << "\",\n";
    stream << "  \"system\": [";
    const auto systems = rq_normalize_systems(metadata.systems);
    for (std::size_t index = 0; index < systems.size(); ++index) {
        if (index > 0) {
            stream << ", ";
        }
        stream << "\"" << json_escape(systems[index]) << "\"";
    }
    stream << "],\n";
    stream << "  \"vendor\": \"" << json_escape(metadata.vendor) << "\",\n";
    stream << "  \"maintainerEmail\": \"" << json_escape(metadata.maintainerEmail) << "\",\n";
    stream << "  \"url\": \"" << json_escape(metadata.url) << "\"";

    if (!metadata.tags.empty()) {
        stream << ",\n  \"tags\": [";
        for (std::size_t index = 0; index < metadata.tags.size(); ++index) {
            if (index > 0) {
                stream << ", ";
            }
            stream << "\"" << json_escape(metadata.tags[index]) << "\"";
        }
        stream << ']';
    }
    if (!metadata.homepage.empty()) {
        stream << ",\n  \"homepage\": \"" << json_escape(metadata.homepage) << "\"";
    }
    if (!metadata.sourceUrl.empty()) {
        stream << ",\n  \"sourceUrl\": \"" << json_escape(metadata.sourceUrl) << "\"";
    }
    if (!metadata.packager.empty()) {
        stream << ",\n  \"packager\": \"" << json_escape(metadata.packager) << "\"";
    }
    if (!metadata.buildDate.empty()) {
        stream << ",\n  \"buildDate\": \"" << json_escape(metadata.buildDate) << "\"";
    }
    if (!metadata.depends.empty()) {
        stream << ",\n  \"depends\": [";
        for (std::size_t index = 0; index < metadata.depends.size(); ++index) {
            if (index > 0) {
                stream << ", ";
            }
            stream << "\"" << json_escape(metadata.depends[index]) << "\"";
        }
        stream << ']';
    }
    if (!metadata.provides.empty()) {
        stream << ",\n  \"provides\": [";
        for (std::size_t index = 0; index < metadata.provides.size(); ++index) {
            if (index > 0) {
                stream << ", ";
            }
            stream << "\"" << json_escape(metadata.provides[index]) << "\"";
        }
        stream << ']';
    }
    if (!metadata.conflicts.empty()) {
        stream << ",\n  \"conflicts\": [";
        for (std::size_t index = 0; index < metadata.conflicts.size(); ++index) {
            if (index > 0) {
                stream << ", ";
            }
            stream << "\"" << json_escape(metadata.conflicts[index]) << "\"";
        }
        stream << ']';
    }
    if (!metadata.replaces.empty()) {
        stream << ",\n  \"replaces\": [";
        for (std::size_t index = 0; index < metadata.replaces.size(); ++index) {
            if (index > 0) {
                stream << ", ";
            }
            stream << "\"" << json_escape(metadata.replaces[index]) << "\"";
        }
        stream << ']';
    }
    if (metadata.payload.has_value()) {
        const auto& payload = metadata.payload.value();
        stream << ",\n  \"payload\": {\n";
        stream << "    \"path\": \"" << json_escape(payload.path) << "\",\n";
        stream << "    \"archive\": \"" << json_escape(payload.archive) << "\",\n";
        stream << "    \"compression\": \"" << json_escape(payload.compression) << "\",\n";
        stream << "    \"hashAlgorithm\": \"" << json_escape(payload.hashAlgorithm) << "\",\n";
        stream << "    \"hashFile\": \"" << json_escape(payload.hashFile) << "\",\n";
        stream << "    \"sizeCompressed\": " << payload.sizeCompressed << ",\n";
        stream << "    \"sizeInstalledExpected\": " << payload.sizeInstalledExpected << "\n";
        stream << "  }";
    }

    stream << "\n}\n";
    return stream.str();
}

std::map<std::string, std::string> parse_reqpack_hooks_impl(const std::filesystem::path& reqpackLuaPath) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::package, sol::lib::table, sol::lib::string, sol::lib::math);

    sol::load_result loadResult = lua.load_file(reqpackLuaPath.string());
    if (!loadResult.valid()) {
        const sol::error err = loadResult;
        throw std::runtime_error("failed to parse reqpack.lua: " + std::string(err.what()));
    }

    const sol::protected_function_result execResult = loadResult();
    if (!execResult.valid()) {
        const sol::error err = execResult;
        throw std::runtime_error("failed to execute reqpack.lua: " + std::string(err.what()));
    }

    sol::table packageTable;
    if (execResult.return_count() > 0 && execResult.get_type() == sol::type::table) {
        packageTable = execResult;
    } else {
        const sol::object packageObject = lua["package"];
        if (packageObject.get_type() == sol::type::table) {
            packageTable = packageObject.as<sol::table>();
        }
    }

    if (!packageTable.valid()) {
        throw std::runtime_error("reqpack.lua has no package table");
    }

    const sol::optional<int> apiVersion = packageTable["apiVersion"];
    if (!apiVersion.has_value() || apiVersion.value() != 1) {
        throw std::runtime_error("reqpack.lua apiVersion missing or unsupported");
    }

    std::map<std::string, std::string> hooks;
    const sol::object hooksObject = packageTable["hooks"];
    if (hooksObject.valid() && hooksObject.get_type() == sol::type::table) {
        const sol::table hookTable = hooksObject.as<sol::table>();
        for (const auto& [key, value] : hookTable) {
            if (key.get_type() != sol::type::string || value.get_type() != sol::type::string) {
                continue;
            }
            hooks[key.as<std::string>()] = value.as<std::string>();
        }
    }

    if (!hooks.contains("install")) {
        throw std::runtime_error("reqpack.lua missing hooks.install");
    }

    return hooks;
}

}  // namespace rq_package_internal
