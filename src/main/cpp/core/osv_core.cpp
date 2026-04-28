#include "core/osv_core.h"

#include "core/version_compare.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace {

using boost::property_tree::ptree;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return {};
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
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

double score_from_string(const std::string& value) {
    try {
        std::size_t start = value.find_first_of("0123456789");
        if (start == std::string::npos) {
            return 0.0;
        }

        std::size_t end = start;
        while (end < value.size() && (std::isdigit(static_cast<unsigned char>(value[end])) || value[end] == '.')) {
            ++end;
        }
        return std::stod(value.substr(start, end - start));
    } catch (...) {
        return 0.0;
    }
}

std::string severity_from_score(const double score) {
    if (score >= 9.0) {
        return "critical";
    }
    if (score >= 7.0) {
        return "high";
    }
    if (score >= 4.0) {
        return "medium";
    }
    if (score > 0.0) {
        return "low";
    }
    return "unassigned";
}

void update_severity_and_score(const ptree& severityNode, std::string& severity, double& score) {
    for (const auto& [_, item] : severityNode) {
        const std::string value = item.get<std::string>("score", {});
        const double parsedScore = score_from_string(value);
        if (parsedScore > score) {
            score = parsedScore;
            severity = severity_from_score(parsedScore);
        }
    }
}

bool version_in_events(const std::vector<OsvRangeEvent>& events, const std::string& version, const VersionComparatorSpec& comparator) {
    if (version.empty()) {
        return false;
    }

    bool inRange = false;
    for (const OsvRangeEvent& event : events) {
        if (!event.introduced.empty()) {
            if (event.introduced == "0" || version_greater_equal(version, event.introduced, comparator)) {
                inRange = true;
            }
        }
        if (!event.fixed.empty() && version_greater_equal(version, event.fixed, comparator)) {
            inRange = false;
        }
        if (!event.lastAffected.empty() && version_less_equal(version, event.lastAffected, comparator)) {
            inRange = true;
        }
        if (!event.limit.empty() && !version_less_equal(version, event.limit, comparator)) {
            inRange = false;
        }
    }

    return inRange;
}

}  // namespace

std::string osv_to_lower_copy(const std::string& value) {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return normalized;
}

std::string osv_normalize_package_name(const std::string& value) {
    return osv_to_lower_copy(value);
}

std::string osv_package_index_key(const std::string& ecosystem, const std::string& name) {
    return ecosystem + "\n" + osv_normalize_package_name(name);
}

std::optional<OsvAdvisory> osv_parse_advisory(const std::string& json) {
    const auto parsed = parse_json_tree(json);
    if (!parsed.has_value()) {
        return std::nullopt;
    }

    const ptree& tree = parsed.value();
    OsvAdvisory advisory;
    advisory.id = tree.get<std::string>("id", {});
    advisory.modified = tree.get<std::string>("modified", {});
    advisory.published = tree.get<std::string>("published", {});
    advisory.summary = tree.get<std::string>("summary", {});
    advisory.details = tree.get<std::string>("details", {});
    advisory.withdrawn = !tree.get<std::string>("withdrawn", {}).empty();
    advisory.rawJson = json;

    if (advisory.id.empty()) {
        return std::nullopt;
    }

    if (const auto aliases = tree.get_child_optional("aliases")) {
        for (const auto& [_, item] : aliases.value()) {
            advisory.aliases.push_back(item.get_value<std::string>());
        }
    }

    if (const auto severities = tree.get_child_optional("severity")) {
        update_severity_and_score(severities.value(), advisory.severity, advisory.score);
    }

    if (const auto affected = tree.get_child_optional("affected")) {
        for (const auto& [_, affectedItem] : affected.value()) {
            OsvAffectedPackage package;
            package.ecosystem = affectedItem.get<std::string>("package.ecosystem", {});
            package.name = affectedItem.get<std::string>("package.name", {});
            package.severity = advisory.severity;
            package.score = advisory.score;

            if (const auto packageSeverities = affectedItem.get_child_optional("severity")) {
                update_severity_and_score(packageSeverities.value(), package.severity, package.score);
            }

            if (const auto versions = affectedItem.get_child_optional("versions")) {
                for (const auto& [_, item] : versions.value()) {
                    package.versions.push_back(item.get_value<std::string>());
                }
            }

            if (const auto ranges = affectedItem.get_child_optional("ranges")) {
                for (const auto& [_, range] : ranges.value()) {
                    if (const auto events = range.get_child_optional("events")) {
                        for (const auto& [_, eventNode] : events.value()) {
                            OsvRangeEvent event;
                            event.introduced = eventNode.get<std::string>("introduced", {});
                            event.fixed = eventNode.get<std::string>("fixed", {});
                            event.lastAffected = eventNode.get<std::string>("last_affected", {});
                            event.limit = eventNode.get<std::string>("limit", {});
                            package.events.push_back(std::move(event));
                        }
                    }
                }
            }

            advisory.affected.push_back(std::move(package));
        }
    }

    return advisory;
}

std::vector<OsvAdvisory> osv_load_advisories_from_path(const std::filesystem::path& path) {
    std::vector<OsvAdvisory> advisories;
    const std::string content = read_file(path);
    if (content.empty()) {
        return advisories;
    }

    const auto parsed = parse_json_tree(content);
    if (!parsed.has_value()) {
        return advisories;
    }

    const ptree& tree = parsed.value();
    if (const std::string id = tree.get<std::string>("id", {}); !id.empty()) {
        const auto advisory = osv_parse_advisory(content);
        if (advisory.has_value()) {
            advisories.push_back(advisory.value());
        }
        return advisories;
    }

    for (const auto& [_, item] : tree) {
        std::ostringstream output;
        try {
            boost::property_tree::write_json(output, item, false);
        } catch (...) {
            continue;
        }
        const auto advisory = osv_parse_advisory(output.str());
        if (advisory.has_value()) {
            advisories.push_back(advisory.value());
        }
    }

    return advisories;
}

std::vector<std::pair<std::string, std::string>> osv_collect_package_keys(const OsvAdvisory& advisory) {
    std::vector<std::pair<std::string, std::string>> keys;
    for (const OsvAffectedPackage& package : advisory.affected) {
        if (package.ecosystem.empty() || package.name.empty()) {
            continue;
        }
        keys.emplace_back(package.ecosystem, osv_normalize_package_name(package.name));
    }
    return keys;
}

std::optional<OsvMatchResult> osv_match_package(
    const OsvAdvisory& advisory,
    const std::string& ecosystem,
    const std::string& name,
    const std::string& version,
    const VersionComparatorSpec& comparator
) {
    const std::string normalizedName = osv_normalize_package_name(name);
    for (const OsvAffectedPackage& package : advisory.affected) {
        if (package.ecosystem != ecosystem || osv_normalize_package_name(package.name) != normalizedName) {
            continue;
        }

        if (!version.empty()) {
            if (std::find(package.versions.begin(), package.versions.end(), version) != package.versions.end() ||
                version_in_events(package.events, version, comparator)) {
                return OsvMatchResult{.severity = package.severity, .score = package.score};
            }
            continue;
        }

        if (!package.versions.empty() || !package.events.empty()) {
            return std::nullopt;
        }

        return OsvMatchResult{.severity = package.severity, .score = package.score};
    }

    return std::nullopt;
}
