#include "cli_parse_request_flags.h"

namespace cli_internal {

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

}  // namespace cli_internal
