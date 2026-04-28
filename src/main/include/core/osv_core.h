#pragma once

#include "core/version_compare.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct OsvRangeEvent {
    std::string introduced;
    std::string fixed;
    std::string lastAffected;
    std::string limit;
};

struct OsvAffectedPackage {
    std::string ecosystem;
    std::string name;
    std::string severity{"unassigned"};
    double score{0.0};
    std::vector<std::string> versions;
    std::vector<OsvRangeEvent> events;
};

struct OsvAdvisory {
    std::string id;
    std::vector<std::string> aliases;
    std::string modified;
    std::string published;
    bool withdrawn{false};
    std::string summary;
    std::string details;
    std::string severity{"unassigned"};
    double score{0.0};
    std::vector<OsvAffectedPackage> affected;
    std::string rawJson;
};

struct OsvMatchResult {
    std::string severity{"unassigned"};
    double score{0.0};
};

std::string osv_to_lower_copy(const std::string& value);
std::string osv_normalize_package_name(const std::string& value);
std::string osv_package_index_key(const std::string& ecosystem, const std::string& name);
std::optional<OsvAdvisory> osv_parse_advisory(const std::string& json);
std::vector<OsvAdvisory> osv_load_advisories_from_path(const std::filesystem::path& path);
std::vector<std::pair<std::string, std::string>> osv_collect_package_keys(const OsvAdvisory& advisory);
std::optional<OsvMatchResult> osv_match_package(
    const OsvAdvisory& advisory,
    const std::string& ecosystem,
    const std::string& name,
    const std::string& version,
    const VersionComparatorSpec& comparator = {}
);
