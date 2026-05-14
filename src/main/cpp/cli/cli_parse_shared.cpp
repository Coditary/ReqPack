#include "cli_parse_shared.h"

#include "cli/cli.h"
#include "core/manifest/manifest_loader.h"

#include <algorithm>
#include <cctype>

namespace {

bool is_install_command_token(const std::string& normalizedCommand) {
    return normalizedCommand == "install" || normalizedCommand == "i";
}

bool is_remove_command_token(const std::string& normalizedCommand) {
    return normalizedCommand == "remove" || normalizedCommand == "rm";
}

bool is_update_command_token(const std::string& normalizedCommand) {
    return normalizedCommand == "update" || normalizedCommand == "up";
}

bool matches_flag_name(const std::string& argument, const std::string& name) {
    return argument == name || argument.rfind(name + "=", 0) == 0;
}

}  // namespace

namespace cli_internal {

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

ActionType parse_action_token(const std::string& command) {
    const std::string normalizedCommand = to_lower_copy(command);

    if (is_install_command_token(normalizedCommand)) {
        return ActionType::INSTALL;
    }
    if (is_remove_command_token(normalizedCommand)) {
        return ActionType::REMOVE;
    }
    if (is_update_command_token(normalizedCommand)) {
        return ActionType::UPDATE;
    }
    if (normalizedCommand == "search") {
        return ActionType::SEARCH;
    }
    if (normalizedCommand == "list") {
        return ActionType::LIST;
    }
    if (normalizedCommand == "info") {
        return ActionType::INFO;
    }
    if (normalizedCommand == "ensure") {
        return ActionType::ENSURE;
    }
    if (normalizedCommand == "sbom") {
        return ActionType::SBOM;
    }
    if (normalizedCommand == "audit") {
        return ActionType::AUDIT;
    }
    if (normalizedCommand == "outdated") {
        return ActionType::OUTDATED;
    }
    if (normalizedCommand == "host") {
        return ActionType::HOST;
    }
    if (normalizedCommand == "snapshot") {
        return ActionType::SNAPSHOT;
    }
    if (normalizedCommand == "pack") {
        return ActionType::PACK;
    }
    if (normalizedCommand == "serve") {
        return ActionType::SERVE;
    }
    if (normalizedCommand == "remote") {
        return ActionType::REMOTE;
    }

    return ActionType::UNKNOWN;
}

bool is_flag_argument(const std::string& argument) {
    return argument.rfind("--", 0) == 0 && argument.size() > 2;
}

bool is_help_flag_argument(const std::string& argument) {
    return argument == "--help" || argument == "-h";
}

std::optional<std::pair<std::string, std::string>> split_scoped_package_argument(
    const std::string& argument,
    const std::set<std::string>& known_systems
) {
    const std::size_t separator = argument.find(':');
    if (separator == std::string::npos || separator == 0 || separator == argument.size() - 1) {
        return std::nullopt;
    }

    const std::string system = to_lower_copy(argument.substr(0, separator));
    if (!known_systems.contains(system)) {
        return std::nullopt;
    }

    return std::make_pair(system, argument.substr(separator + 1));
}

bool is_existing_path(const std::string& value) {
    std::error_code error;
    return std::filesystem::exists(std::filesystem::path(value), error) && !error;
}

bool is_existing_regular_file(const std::string& value) {
    std::error_code error;
    return std::filesystem::is_regular_file(std::filesystem::path(value), error) && !error;
}

bool is_url(const std::string& value) {
    return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0 || value.rfind("file://", 0) == 0;
}

bool supports_manifest_path(ActionType action) {
    return action == ActionType::INSTALL || action == ActionType::REMOVE || action == ActionType::UPDATE || action == ActionType::AUDIT ||
           action == ActionType::LIST || action == ActionType::OUTDATED;
}

std::optional<std::filesystem::path> resolve_manifest_path_argument(const std::string& argument) {
    if (argument.empty()) {
        return std::nullopt;
    }

    const bool explicitPath = argument[0] == '.' || argument[0] == '/';
    const bool bareManifestFilename = std::filesystem::path(argument).filename() == MANIFEST_FILENAME;
    if (!explicitPath && !bareManifestFilename) {
        return std::nullopt;
    }

    std::error_code fsError;
    const std::filesystem::path candidatePath = std::filesystem::absolute(std::filesystem::path(argument), fsError);
    if (fsError) {
        return std::nullopt;
    }

    if (std::filesystem::is_directory(candidatePath, fsError) && !fsError) {
        return candidatePath / MANIFEST_FILENAME;
    }

    fsError.clear();
    if (std::filesystem::is_regular_file(candidatePath, fsError) && !fsError && candidatePath.filename() == MANIFEST_FILENAME) {
        return candidatePath;
    }

    return std::nullopt;
}

std::optional<AuditOutputFormat> infer_audit_output_format_from_path(const std::string& path) {
    const std::string extension = to_lower_copy(std::filesystem::path(path).extension().string());
    if (extension == ".sarif") {
        return AuditOutputFormat::SARIF;
    }
    if (!extension.empty()) {
        return AuditOutputFormat::CYCLONEDX_VEX_JSON;
    }
    return std::nullopt;
}

bool consume_package_result_filter_flag(
    ActionType action,
    const std::vector<std::string>& arguments,
    std::size_t& index,
    std::vector<std::string>& flags
) {
    if (action != ActionType::SEARCH && action != ActionType::LIST && action != ActionType::OUTDATED) {
        return false;
    }

    const std::string& argument = arguments[index];
    const char* key = nullptr;
    if (argument == "--arch") {
        key = "arch";
    } else if (argument == "--type") {
        key = "type";
    } else {
        return false;
    }

    if (index + 1 >= arguments.size() || arguments[index + 1].empty() ||
        (arguments[index + 1].rfind("--", 0) == 0 && arguments[index + 1].size() > 2)) {
        return true;
    }

    flags.push_back(std::string(key) + "=" + to_lower_copy(arguments[++index]));
    return true;
}

bool has_flag(const std::vector<std::string>& flags, const std::string& name) {
    return std::find(flags.begin(), flags.end(), name) != flags.end();
}

bool update_command_has_package_mode_flag(const std::vector<std::string>& arguments) {
    bool updateSeen = false;
    for (const std::string& argument : arguments) {
        if (!updateSeen) {
            if (is_update_command_token(to_lower_copy(argument))) {
                updateSeen = true;
            }
            continue;
        }

        if (matches_flag_name(argument, "--dry-run") || matches_flag_name(argument, "--prompt-on-unsafe") ||
            matches_flag_name(argument, "--abort-on-unsafe") || matches_flag_name(argument, "--severity-threshold") ||
            matches_flag_name(argument, "--score-threshold") || matches_flag_name(argument, "--jobs") ||
            matches_flag_name(argument, "--jobs-max") || matches_flag_name(argument, "--stop-on-first-failure")) {
            return true;
        }
    }
    return false;
}

bool is_removed_security_backend_flag(const std::string& argument) {
    return argument == "--snyk" || argument == "--owasp";
}

bool current_system_prefers_package_tokens(const std::string& currentSystem, ActionType action) {
    const std::string normalizedSystem = to_lower_copy(currentSystem);
    if (normalizedSystem != "sys" && normalizedSystem != "rqp") {
        return false;
    }

    switch (action) {
        case ActionType::INSTALL:
        case ActionType::REMOVE:
        case ActionType::UPDATE:
        case ActionType::SEARCH:
        case ActionType::INFO:
        case ActionType::SBOM:
        case ActionType::AUDIT:
        case ActionType::PACK:
            return true;
        default:
            return false;
    }
}

}  // namespace cli_internal

ActionType Cli::parse_action(const std::string& command) {
    return cli_internal::parse_action_token(command);
}

bool Cli::is_flag(const std::string& argument) {
    return cli_internal::is_flag_argument(argument);
}

bool Cli::is_help_flag(const std::string& argument) {
    return cli_internal::is_help_flag_argument(argument);
}

std::optional<std::pair<std::string, std::string>> Cli::split_scoped_package(
    const std::string& argument,
    const std::set<std::string>& known_systems
) {
    return cli_internal::split_scoped_package_argument(argument, known_systems);
}
