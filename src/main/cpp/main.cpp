#include "cli/cli.h"
#include "core/configuration.h"
#include "core/orchestrator.h"
#include "core/remote_client.h"
#include "core/serve_remote.h"
#include "output/display_factory.h"
#include "output/logger.h"
#include "plugins/lua_bridge.h"

#include <curl/curl.h>

#include <cctype>
#include <filesystem>
#include <iostream>
#include <optional>

namespace {

struct StdinCommand {
    std::size_t lineNumber{0};
    std::string text;
};

struct RemoteClientInvocation {
    std::string profileName;
    std::vector<std::string> forwardedArguments;
};

std::string trim_copy(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<StdinCommand> read_stdin_commands(std::istream& input) {
    std::vector<StdinCommand> commands;
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        const std::string trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed.rfind("#", 0) == 0) {
            continue;
        }
        commands.push_back(StdinCommand{.lineNumber = lineNumber, .text = trimmed});
    }
    return commands;
}

std::vector<std::string> tokenize_command_line(const std::string& command) {
    std::vector<std::string> tokens;
    std::string current;
    bool inSingleQuotes = false;
    bool inDoubleQuotes = false;
    bool escaping = false;

    for (char c : command) {
        if (escaping) {
            current.push_back(c);
            escaping = false;
            continue;
        }

        if (c == '\\' && !inSingleQuotes) {
            escaping = true;
            continue;
        }

        if (c == '\'' && !inDoubleQuotes) {
            inSingleQuotes = !inSingleQuotes;
            continue;
        }

        if (c == '"' && !inSingleQuotes) {
            inDoubleQuotes = !inDoubleQuotes;
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(c)) && !inSingleQuotes && !inDoubleQuotes) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }

        current.push_back(c);
    }

    if (escaping || inSingleQuotes || inDoubleQuotes) {
        return {};
    }

    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }

    return tokens;
}

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

bool is_install_stdin_command(const std::vector<std::string>& arguments) {
    const std::vector<std::string> filtered = strip_config_arguments(arguments);
    return filtered.size() >= 2 && filtered.front() == "install" && contains_flag(filtered, "--stdin");
}

std::vector<std::string> inherited_stream_arguments(const std::vector<std::string>& arguments) {
    const std::vector<std::string> filtered = strip_config_arguments(arguments);
    std::vector<std::string> inherited;
    inherited.reserve(filtered.size());
    bool actionSeen = false;

    for (const std::string& argument : filtered) {
        if (!actionSeen) {
            if (argument == "install" || argument == "serve") {
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

bool parse_serve_runtime_options(
    const std::vector<std::string>& arguments,
    ServeRuntimeOptions& options,
    std::string& error
) {
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

bool parse_remote_client_invocation(
    const std::vector<std::string>& arguments,
    RemoteClientInvocation& invocation,
    std::string& error
) {
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

std::vector<std::string> merged_stream_command_arguments(
    const std::vector<std::string>& commandTokens,
    const std::vector<std::string>& inheritedArguments
) {
    std::vector<std::string> merged;
    merged.reserve(commandTokens.size() + inheritedArguments.size());
    merged.insert(merged.end(), commandTokens.begin(), commandTokens.end());
    merged.insert(merged.end(), inheritedArguments.begin(), inheritedArguments.end());
    return merged;
}

int run_stdin_install_batch(Cli& cli, const ReqPackConfig& config, const std::vector<std::string>& inheritedArguments) {
    Logger& logger = Logger::instance();
    const std::vector<StdinCommand> commands = read_stdin_commands(std::cin);
    std::vector<Request> requests;
    ReqPackConfig effectiveConfig = apply_config_overrides(config, extract_cli_config_overrides(inheritedArguments));

    for (const StdinCommand& command : commands) {
        const std::vector<std::string> commandTokens = tokenize_command_line(command.text);
        if (commandTokens.empty()) {
            logger.err("stdin line " + std::to_string(command.lineNumber) + ": invalid command syntax");
            return 1;
        }

        const std::vector<std::string> mergedTokens = merged_stream_command_arguments(commandTokens, inheritedArguments);
        effectiveConfig = apply_config_overrides(effectiveConfig, extract_cli_config_overrides(mergedTokens));
        const std::vector<Request> parsed = cli.parse(mergedTokens, effectiveConfig);
        if (parsed.empty()) {
            logger.err("stdin line " + std::to_string(command.lineNumber) + ": failed to parse '" + command.text + "'");
            return 1;
        }

        for (const Request& request : parsed) {
            if (request.action != ActionType::INSTALL) {
                logger.err("stdin line " + std::to_string(command.lineNumber) + ": only install commands allowed in 'install --stdin'");
                return 1;
            }
            requests.push_back(request);
        }
    }

    if (requests.empty()) {
        logger.err("stdin contained no install commands");
        return 1;
    }

    Orchestrator orchestrator(std::move(requests), effectiveConfig);
    const int result = orchestrator.run();
    logger.flushSync();
    return result;
}

int run_stdin_serve_loop(Cli& cli, const ReqPackConfig& config, const std::vector<std::string>& inheritedArguments) {
    Logger& logger = Logger::instance();
    int exitCode = 0;

    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(std::cin, line)) {
        ++lineNumber;
        const std::string trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed.rfind("#", 0) == 0) {
            continue;
        }

        const std::vector<std::string> commandTokens = tokenize_command_line(trimmed);
        if (commandTokens.empty()) {
            logger.err("stdin line " + std::to_string(lineNumber) + ": invalid command syntax");
            exitCode = 1;
            continue;
        }

        const std::vector<std::string> mergedTokens = merged_stream_command_arguments(commandTokens, inheritedArguments);
        const ReqPackConfig effectiveConfig = apply_config_overrides(config, extract_cli_config_overrides(mergedTokens));
        const std::vector<Request> requests = cli.parse(mergedTokens, effectiveConfig);
        if (requests.empty()) {
            logger.err("stdin line " + std::to_string(lineNumber) + ": failed to parse '" + trimmed + "'");
            exitCode = 1;
            continue;
        }

        Orchestrator orchestrator(requests, effectiveConfig);
        if (orchestrator.run() != 0) {
            exitCode = 1;
        }
        logger.flushSync();
    }

    return exitCode;
}

}  // namespace

int main(int argc, char* argv[]) {
    // Fast path: handle -h/--help before any heavy initialisation.
    // This avoids CLI11 internal-state issues and the async Logger worker hang.
    {
        Cli earlyCliCheck;
        if (earlyCliCheck.handleHelp(argc, argv)) {
            return 0;
        }
    }

    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
        return 1;
    }

    Cli cli;
    const ReqPackConfigOverrides configOverrides = cli.parseConfigOverrides(argc, argv);
    const std::filesystem::path configPath = configOverrides.configPath.value_or(default_reqpack_config_path());
    const ReqPackConfig defaults = default_reqpack_config();
    const ReqPackConfig fileConfig = load_config_from_lua(configPath, defaults);
    ReqPackConfig config = apply_config_overrides(fileConfig, configOverrides);
    const std::filesystem::path workspacePluginDirectory = std::filesystem::current_path() / "plugins";
    if (!configOverrides.pluginDirectory.has_value() &&
        config.registry.pluginDirectory == defaults.registry.pluginDirectory &&
        std::filesystem::exists(workspacePluginDirectory)) {
        config.registry.pluginDirectory = workspacePluginDirectory.string();
    }
    Logger& logger = Logger::instance();

    logger.setLevel(to_string(config.logging.level));
    logger.setPattern(config.logging.pattern);
    logger.setBacktrace(config.logging.enableBacktrace, config.logging.backtraceSize);
    logger.setConsoleOutput(config.logging.consoleOutput);
    if (config.logging.fileOutput) {
        logger.setFileSink(config.logging.filePath);
    }

    std::unique_ptr<IDisplay> display = create_display(config.display);
    logger.setDisplay(display.get());

    std::vector<std::string> rawArguments;
    rawArguments.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) {
        rawArguments.emplace_back(argv[i]);
    }

    ServeRuntimeOptions serveOptions;
    std::string serveError;
    const bool isServeCommand = parse_serve_runtime_options(rawArguments, serveOptions, serveError);
    if (isServeCommand) {
        if (!serveError.empty()) {
            logger.err(serveError);
            logger.flushSync();
            curl_global_cleanup();
            return 1;
        }
        if (!serveOptions.readonlyExplicit) {
            serveOptions.readonly = config.remote.readonly;
        }
        if (!serveOptions.maxConnectionsExplicit) {
            serveOptions.maxConnections = config.remote.maxConnections;
        }
        if (serveOptions.stdin) {
            const int result = run_stdin_serve_loop(cli, config, serveOptions.inheritedArguments);
            curl_global_cleanup();
            return result;
        }
        const int result = run_remote_serve(cli, config, configPath, configOverrides, logger, display.get(), serveOptions);
        curl_global_cleanup();
        return result;
    }

    RemoteClientInvocation remoteInvocation;
    std::string remoteError;
    const bool isRemoteCommand = parse_remote_client_invocation(rawArguments, remoteInvocation, remoteError);
    if (isRemoteCommand) {
        if (!remoteError.empty()) {
            logger.err(remoteError);
            logger.flushSync();
            curl_global_cleanup();
            return 1;
        }

        int result = 1;
        try {
			result = run_remote_client(config, default_remote_profiles_path(), remoteInvocation.profileName, remoteInvocation.forwardedArguments, display.get());
        } catch (const std::exception& e) {
            logger.err(e.what());
            logger.flushSync();
            curl_global_cleanup();
            return 1;
        }

        curl_global_cleanup();
        return result;
    }

    if (is_install_stdin_command(rawArguments)) {
        const int result = run_stdin_install_batch(cli, config, inherited_stream_arguments(rawArguments));
        curl_global_cleanup();
        return result;
    }

    const std::vector<Request> requests = cli.parse(argc, argv, config);

    if (requests.empty()) {
        if (cli.parseFailed()) {
            logger.flushSync();
            curl_global_cleanup();
            return 1;
        }
        cli.print_help();
        logger.flushSync();
        curl_global_cleanup();
        return 0;
    }

    Orchestrator orchestrator(requests, config);
    const int result = orchestrator.run();
    logger.flushSync();

    curl_global_cleanup();
    return result;
}
