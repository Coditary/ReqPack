#include "main_arg_parsing.h"

#include "cli/cli.h"
#include "core/config/configuration.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

namespace {

bool contains_flag(const std::vector<std::string>& arguments, const std::string& flag) {
    for (const std::string& argument : arguments) {
        if (argument == flag) {
            return true;
        }
    }
    return false;
}

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::vector<std::string> strip_config_arguments(const std::vector<std::string>& arguments) {
    std::vector<std::string> filtered;
    filtered.reserve(arguments.size());
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        ReqPackConfigOverrides ignoredOverrides;
        std::size_t configIndex = i;
        if (consume_cli_config_flag(arguments, configIndex, ignoredOverrides)) {
            i = configIndex;
            continue;
        }
        filtered.push_back(arguments[i]);
    }
    return filtered;
}

bool parse_int_flag_value(
    const std::vector<std::string>& arguments,
    std::size_t& index,
    const std::string& argument,
    const std::string& flag,
    int& target,
    std::string& error
) {
    std::string value;
    if (argument == flag) {
        if (index + 1 >= arguments.size()) {
            error = flag + " requires a value";
            return false;
        }
        value = arguments[++index];
    } else if (starts_with(argument, flag + "=")) {
        value = argument.substr(flag.size() + 1);
    } else {
        return true;
    }

    try {
        target = std::stoi(value);
    } catch (...) {
        error = "invalid numeric value for " + flag + ": " + value;
        return false;
    }
    return true;
}

bool parse_string_flag_value(
    const std::vector<std::string>& arguments,
    std::size_t& index,
    const std::string& argument,
    const std::string& flag,
    std::string& target,
    std::string& error
) {
    if (argument == flag) {
        if (index + 1 >= arguments.size()) {
            error = flag + " requires a value";
            return false;
        }
        target = arguments[++index];
        return true;
    }
    if (starts_with(argument, flag + "=")) {
        target = argument.substr(flag.size() + 1);
    }
    return true;
}

bool parse_optional_string_flag_value(
    const std::vector<std::string>& arguments,
    std::size_t& index,
    const std::string& argument,
    const std::string& flag,
    std::optional<std::string>& target,
    std::string& error
) {
    std::string value;
    if (argument == flag) {
        if (index + 1 >= arguments.size()) {
            error = flag + " requires a value";
            return false;
        }
        value = arguments[++index];
    } else if (starts_with(argument, flag + "=")) {
        value = argument.substr(flag.size() + 1);
    } else {
        return true;
    }
    target = value;
    return true;
}

}  // namespace

bool parse_serve_runtime_options(const std::vector<std::string>& arguments,
                                 ServeRuntimeOptions& options,
                                 std::string& error) {
    const std::vector<std::string> filtered = strip_config_arguments(arguments);
    if (filtered.empty() || filtered.front() != "serve") {
        return false;
    }

    options = ServeRuntimeOptions{};

    for (std::size_t i = 1; i < filtered.size(); ++i) {
        const std::string& argument = filtered[i];
        if (argument == "--stdin") {
            options.stdin = true;
            continue;
        }
        if (argument == "--remote") {
            options.remote = true;
            continue;
        }
        if (argument == "--json") {
            options.remoteProtocol = ServeRemoteProtocol::JSON;
            continue;
        }
        if (argument == "--http") {
            options.remoteProtocol = ServeRemoteProtocol::HTTP;
            continue;
        }
        if (argument == "--https") {
            options.remoteProtocol = ServeRemoteProtocol::HTTPS;
            continue;
        }
        if (argument == "--readonly") {
            options.readonly = true;
            options.readonlyExplicit = true;
            continue;
        }
        if (argument == "--bind" || starts_with(argument, "--bind=")) {
            if (!parse_string_flag_value(filtered, i, argument, "--bind", options.bind, error)) {
                return true;
            }
            continue;
        }
        if (argument == "--port" || starts_with(argument, "--port=")) {
            if (!parse_int_flag_value(filtered, i, argument, "--port", options.port, error)) {
                return true;
            }
            continue;
        }
        if (argument == "--max-connections" || starts_with(argument, "--max-connections=")) {
            if (!parse_int_flag_value(filtered, i, argument, "--max-connections", options.maxConnections, error)) {
                return true;
            }
            options.maxConnectionsExplicit = true;
            continue;
        }
        if (argument == "--token" || starts_with(argument, "--token=")) {
            if (!parse_optional_string_flag_value(filtered, i, argument, "--token", options.token, error)) {
                return true;
            }
            continue;
        }
        if (argument == "--username" || starts_with(argument, "--username=")) {
            if (!parse_optional_string_flag_value(filtered, i, argument, "--username", options.username, error)) {
                return true;
            }
            continue;
        }
        if (argument == "--password" || starts_with(argument, "--password=")) {
            if (!parse_optional_string_flag_value(filtered, i, argument, "--password", options.password, error)) {
                return true;
            }
            continue;
        }

        options.inheritedArguments.push_back(argument);
    }

    if (options.stdin == options.remote) {
        error = "serve requires exactly one of --stdin or --remote";
        return true;
    }
    if (options.port <= 0 || options.port > 65535) {
        error = "--port must be between 1 and 65535";
        return true;
    }
    if (options.maxConnections <= 0) {
        error = "--max-connections must be greater than 0";
        return true;
    }
    if (options.stdin) {
        if (options.token.has_value() || options.username.has_value() || options.password.has_value() ||
            options.bind != "127.0.0.1" || options.port != 4545 || options.remoteProtocol != ServeRemoteProtocol::TEXT ||
            options.maxConnections != 16) {
            error = "remote-only flags require --remote";
            return true;
        }
    }
    if (options.token.has_value() && (options.username.has_value() || options.password.has_value())) {
        error = "use either --token or --username/--password, not both";
        return true;
    }
    if (options.username.has_value() != options.password.has_value()) {
        error = "--username and --password must be used together";
        return true;
    }
    if (!options.remote && options.remoteProtocol != ServeRemoteProtocol::TEXT) {
        error = "protocol flags require --remote";
        return true;
    }

    return true;
}

bool parse_remote_client_invocation(const std::vector<std::string>& arguments,
                                    RemoteClientInvocation& invocation,
                                    std::string& error) {
    invocation = RemoteClientInvocation{};

    std::size_t actionIndex = arguments.size();
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        ReqPackConfigOverrides ignoredOverrides;
        std::size_t configIndex = i;
        if (consume_cli_config_flag(arguments, configIndex, ignoredOverrides)) {
            i = configIndex;
            continue;
        }
        actionIndex = i;
        break;
    }

    if (actionIndex >= arguments.size() || arguments[actionIndex] != "remote") {
        return false;
    }
    if (actionIndex + 1 >= arguments.size()) {
        error = "remote requires a profile name";
        return true;
    }

    invocation.profileName = arguments[actionIndex + 1];
    invocation.forwardedArguments.assign(arguments.begin() + static_cast<std::ptrdiff_t>(actionIndex + 2), arguments.end());
    return true;
}

bool is_action_stdin_command(const std::vector<std::string>& arguments, const std::string& action) {
    const std::vector<std::string> filtered = strip_config_arguments(arguments);
    return filtered.size() >= 2 && Cli::parse_action(filtered.front()) == Cli::parse_action(action) && contains_flag(filtered, "--stdin");
}

std::vector<std::string> inherited_stream_arguments(const std::vector<std::string>& arguments) {
    const std::vector<std::string> filtered = strip_config_arguments(arguments);
    std::vector<std::string> inherited;
    inherited.reserve(filtered.size());
    bool actionSeen = false;

    for (const std::string& argument : filtered) {
        if (!actionSeen) {
            const ActionType action = Cli::parse_action(argument);
            if (action == ActionType::INSTALL || action == ActionType::REMOVE || action == ActionType::UPDATE || action == ActionType::SERVE) {
                actionSeen = true;
            }
            continue;
        }
        if (argument == "--stdin") {
            continue;
        }
        inherited.push_back(argument);
    }

    return inherited;
}

bool is_self_update_command(const std::vector<std::string>& arguments) {
    const std::vector<std::string> filtered = strip_config_arguments(arguments);
    if (filtered.empty() || Cli::parse_action(filtered.front()) != ActionType::UPDATE) {
        return false;
    }

    for (std::size_t i = 1; i < filtered.size(); ++i) {
        const std::string& argument = filtered[i];
        if (argument == "--help" || argument == "-h") {
            return false;
        }
        if (argument == "--all") {
            return false;
        }
        if (argument == "--stdin" || argument == "--dry-run" || argument == "--prompt-on-unsafe" ||
            argument == "--abort-on-unsafe" || argument == "--severity-threshold" ||
            argument == "--score-threshold" || argument == "--jobs" || argument == "--jobs-max" ||
            argument == "--stop-on-first-failure") {
            return false;
        }
        if (argument.rfind("--", 0) == 0) {
            continue;
        }
        return false;
    }
    return true;
}

bool is_host_refresh_command(const std::vector<std::string>& arguments) {
    const std::vector<std::string> filtered = strip_config_arguments(arguments);
    if (filtered.size() != 2) {
        return false;
    }
    return to_lower_copy(filtered[0]) == "host" && to_lower_copy(filtered[1]) == "refresh";
}

bool is_version_command(const std::vector<std::string>& arguments) {
    const std::vector<std::string> filtered = strip_config_arguments(arguments);
    if (filtered.size() != 1) {
        return false;
    }
    const std::string command = to_lower_copy(filtered.front());
    return command == "version" || command == "--version";
}

bool is_info_command(const std::vector<std::string>& arguments) {
    const std::vector<std::string> filtered = strip_config_arguments(arguments);
    return filtered.size() == 1 && filtered.front() == "info";
}
