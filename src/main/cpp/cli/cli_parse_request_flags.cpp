#include "cli_parse_request_flags.h"

#include <optional>
#include <vector>

namespace cli_internal {

namespace {

std::optional<int> parse_int_flag_value(const std::string& raw) {
    try {
        std::size_t consumed = 0;
        const int value = std::stoi(raw, &consumed);
        if (consumed != raw.size()) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

bool assign_string_pack_flag(RqPackMetadataOverrides& overrides,
                             std::optional<std::string> RqPackMetadataOverrides::*field,
                             const std::string& value) {
    if (value.empty()) {
        return false;
    }
    overrides.*field = value;
    return true;
}

} // namespace

bool is_request_output_path_flag(const std::string& argument) {
    return argument == "--output" || argument == "-o";
}

bool is_request_force_flag(const std::string& argument) {
    return argument == "--force" || argument == "-f";
}

bool is_request_all_packages_flag(const std::string& argument) {
    return argument == "--all" || argument == "-a";
}

bool is_request_dry_run_flag(const std::string& argument) {
    return argument == "--dry-run" || argument == "-n";
}

bool is_request_plugins_flag(const std::string& argument) {
    return argument == "--plugins";
}

bool action_supports_force_flag(ActionType action) {
    return action == ActionType::AUDIT || action == ActionType::SBOM || action == ActionType::SNAPSHOT ||
           action == ActionType::PACK;
}

bool action_supports_output_path_flag(ActionType action) {
    return action == ActionType::AUDIT || action == ActionType::SBOM || action == ActionType::SNAPSHOT ||
           action == ActionType::PACK;
}

std::optional<std::string> canonical_request_flag_token(ActionType action, const std::string& argument) {
    if (action == ActionType::UPDATE && is_request_all_packages_flag(argument)) {
        return std::string {"all"};
    }
    if (action == ActionType::UPDATE && is_request_plugins_flag(argument)) {
        return std::string {"plugins"};
    }
    if (action_supports_force_flag(action) && is_request_force_flag(argument)) {
        return std::string {"force"};
    }
    if (is_request_dry_run_flag(argument)) {
        return std::string {"dry-run"};
    }
    if (argument.rfind("--", 0) == 0 && argument.size() > 2) {
        return argument.substr(2);
    }
    return std::nullopt;
}

bool consume_pack_metadata_flag(const std::vector<std::string>& arguments,
                                std::size_t& index,
                                RqPackMetadataOverrides& overrides) {
    const std::string& argument = arguments[index];
    auto require_value = [&](std::optional<std::string> RqPackMetadataOverrides::*field) -> bool {
        if (index + 1 >= arguments.size()) {
            return false;
        }
        return assign_string_pack_flag(overrides, field, arguments[++index]);
    };

    if (argument == "--pack-version") {
        return require_value(&RqPackMetadataOverrides::version);
    }
    if (argument == "--pack-vendor") {
        return require_value(&RqPackMetadataOverrides::vendor);
    }
    if (argument == "--pack-maintainer-email") {
        return require_value(&RqPackMetadataOverrides::maintainerEmail);
    }
    if (argument == "--pack-url") {
        return require_value(&RqPackMetadataOverrides::url);
    }
    if (argument == "--pack-homepage") {
        return require_value(&RqPackMetadataOverrides::homepage);
    }
    if (argument == "--pack-source-url") {
        return require_value(&RqPackMetadataOverrides::sourceUrl);
    }
    if (argument == "--pack-architecture" || argument == "--pack-arch") {
        return require_value(&RqPackMetadataOverrides::architecture);
    }
    if (argument == "--pack-system") {
        if (index + 1 >= arguments.size()) {
            return false;
        }
        const std::string& value = arguments[++index];
        if (value.empty()) {
            return false;
        }
        overrides.systems.push_back(value);
        return true;
    }
    if (argument == "--pack-release" || argument == "--pack-revision") {
        if (index + 1 >= arguments.size()) {
            return false;
        }
        const std::optional<int> parsed = parse_int_flag_value(arguments[++index]);
        if (!parsed.has_value()) {
            return false;
        }
        if (argument == "--pack-release") {
            overrides.release = parsed;
        } else {
            overrides.revision = parsed;
        }
        return true;
    }

    return false;
}

}  // namespace cli_internal
