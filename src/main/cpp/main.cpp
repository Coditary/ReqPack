#include "cli/cli.h"
#include "core/configuration.h"
#include "core/orchestrator.h"
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

std::optional<ActionType> detect_stream_mode_action(const std::vector<std::string>& arguments) {
    const std::vector<std::string> filtered = strip_config_arguments(arguments);
    if (!contains_flag(filtered, "--stdin")) {
        return std::nullopt;
    }

    for (const std::string& argument : filtered) {
        const std::string lowered = trim_copy(argument);
        if (lowered == "install") {
            return ActionType::INSTALL;
        }
        if (lowered == "serve") {
            return ActionType::SERVE;
        }
    }

    return std::nullopt;
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

    for (const StdinCommand& command : commands) {
        const std::vector<std::string> commandTokens = tokenize_command_line(command.text);
        if (commandTokens.empty()) {
            logger.err("stdin line " + std::to_string(command.lineNumber) + ": invalid command syntax");
            return 1;
        }

        const std::vector<std::string> mergedTokens = merged_stream_command_arguments(commandTokens, inheritedArguments);
        const std::vector<Request> parsed = cli.parse(mergedTokens, config);
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

    Orchestrator orchestrator(std::move(requests), config);
    orchestrator.run();
    logger.flush();
    return 0;
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
        const std::vector<Request> requests = cli.parse(mergedTokens, config);
        if (requests.empty()) {
            logger.err("stdin line " + std::to_string(lineNumber) + ": failed to parse '" + trimmed + "'");
            exitCode = 1;
            continue;
        }

        Orchestrator orchestrator(requests, config);
        orchestrator.run();
        logger.flush();
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
    const ReqPackConfig fileConfig = load_config_from_lua(configPath, DEFAULT_REQPACK_CONFIG);
    ReqPackConfig config = apply_config_overrides(fileConfig, configOverrides);
    const std::filesystem::path workspacePluginDirectory = std::filesystem::current_path() / "plugins";
    if (!configOverrides.pluginDirectory.has_value() && std::filesystem::exists(workspacePluginDirectory)) {
        config.registry.pluginDirectory = workspacePluginDirectory.string();
    }
    Logger& logger = Logger::instance();

    logger.setLevel(to_string(config.logging.level));
    logger.setPattern(config.logging.pattern);
    logger.setBacktrace(config.logging.enableBacktrace, config.logging.backtraceSize);
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

    const std::optional<ActionType> streamModeAction = detect_stream_mode_action(rawArguments);
    const std::vector<std::string> inheritedArguments = inherited_stream_arguments(rawArguments);

    if (streamModeAction.has_value() && streamModeAction.value() == ActionType::INSTALL) {
        const int result = run_stdin_install_batch(cli, config, inheritedArguments);
        curl_global_cleanup();
        return result;
    }

    if (streamModeAction.has_value() && streamModeAction.value() == ActionType::SERVE) {
        const int result = run_stdin_serve_loop(cli, config, inheritedArguments);
        curl_global_cleanup();
        return result;
    }

    const std::vector<Request> requests = cli.parse(argc, argv, config);

    if (requests.empty()) {
        cli.print_help();
        logger.flush();
        curl_global_cleanup();
        return 0;
    }

    Orchestrator orchestrator(requests, config);
    orchestrator.run();
    logger.flush();

    curl_global_cleanup();
    return 0;
}
