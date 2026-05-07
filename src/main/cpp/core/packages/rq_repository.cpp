#include "core/packages/rq_repository.h"

#include "core/packages/rq_package.h"
#include "core/common/version_compare.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
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

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

int required_int(const ptree& tree, const std::string& key) {
    const auto value = tree.get_optional<int>(key);
    if (!value.has_value()) {
        throw std::runtime_error("repository package missing integer field: " + key);
    }
    return value.value();
}

std::string required_string(const ptree& tree, const std::string& key) {
    const auto value = tree.get_optional<std::string>(key);
    if (!value.has_value() || value->empty()) {
        throw std::runtime_error("repository package missing string field: " + key);
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

bool is_valid_sha256(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isxdigit(ch) != 0;
    });
}

bool is_better_candidate(const RqRepositoryPackage& candidate, const RqRepositoryPackage& currentBest) {
    const int versionComparison = version_compare_values(candidate.version, currentBest.version);
    if (versionComparison != 0) {
        return versionComparison > 0;
    }
    if (candidate.release != currentBest.release) {
        return candidate.release > currentBest.release;
    }
    if (candidate.revision != currentBest.revision) {
        return candidate.revision > currentBest.revision;
    }
    return false;
}

}  // namespace

RqRepositoryIndex rq_repository_parse_index(const std::string& content, const std::string& source) {
    const std::optional<ptree> parsed = parse_json_tree(content);
    if (!parsed.has_value()) {
        throw std::runtime_error("invalid rqp repository index json");
    }

    const ptree& tree = parsed.value();
    if (tree.get<int>("schemaVersion", 0) != 1) {
        throw std::runtime_error("unsupported rqp repository schemaVersion");
    }

    RqRepositoryIndex index;
    index.source = source;

    const auto packagesNode = tree.get_child_optional("packages");
    if (!packagesNode.has_value()) {
        return index;
    }

    for (const auto& [_, child] : packagesNode.value()) {
        RqRepositoryPackage package;
        package.repository = source;
        package.name = required_string(child, "name");
        package.version = required_string(child, "version");
        package.release = required_int(child, "release");
        package.revision = required_int(child, "revision");
        package.architecture = required_string(child, "architecture");
        package.summary = required_string(child, "summary");
        package.url = required_string(child, "url");
        package.packageSha256 = to_lower(child.get<std::string>("packageSha256", {}));
        if (!package.packageSha256.empty() && !is_valid_sha256(package.packageSha256)) {
            throw std::runtime_error("repository packageSha256 is not valid sha256");
        }
        package.tags = load_string_array(child.get_child_optional("tags"));
        index.packages.push_back(std::move(package));
    }

    return index;
}

std::optional<RqRepositoryPackage> rq_repository_resolve_package(
    const std::vector<RqRepositoryIndex>& indexes,
    const std::string& name,
    const std::string& version,
    const std::string& hostArchitecture
) {
    std::optional<RqRepositoryPackage> best;

    for (const RqRepositoryIndex& index : indexes) {
        for (const RqRepositoryPackage& package : index.packages) {
            if (package.name != name) {
                continue;
            }
            if (!version.empty() && package.version != version) {
                continue;
            }
            if (!rq_architecture_matches(package.architecture, hostArchitecture)) {
                continue;
            }
            if (!best.has_value() || is_better_candidate(package, best.value())) {
                best = package;
            }
        }
    }

    return best;
}
