#include "main_stdin.h"

#include "main_diagnostics.h"

#include "core/execution/orchestrator.h"
#include "output/logger.h"

#include <cctype>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

std::string to_lower_copy(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

bool is_known_action_token(const std::string& value) {
    return Cli::parse_action(value) != ActionType::UNKNOWN;
}

std::vector<std::string> merged_stream_command_arguments(const std::vector<std::string>& commandTokens,
                                                         const std::vector<std::string>& inheritedArguments) {
    std::vector<std::string> merged;
    merged.reserve(commandTokens.size() + inheritedArguments.size());
    merged.insert(merged.end(), commandTokens.begin(), commandTokens.end());
    merged.insert(merged.end(), inheritedArguments.begin(), inheritedArguments.end());
    return merged;
}

std::vector<std::string> action_stdin_payload_tokens(const std::vector<std::string>& commandTokens, const std::string& action) {
    if (!commandTokens.empty() && (to_lower_copy(commandTokens.front()) == action || is_known_action_token(commandTokens.front()))) {
        return commandTokens;
    }

    std::vector<std::string> expanded;
    expanded.reserve(commandTokens.size() + 1);
    expanded.push_back(action);
    expanded.insert(expanded.end(), commandTokens.begin(), commandTokens.end());
    return expanded;
}

}  // namespace

int run_stdin_action_batch(Cli& cli,
                           const ReqPackConfig& config,
                           const std::vector<std::string>& inheritedArguments,
                           ActionType expectedAction,
                           const std::string& actionName) {
    Logger& logger = Logger::instance();
    const std::vector<StdinCommand> commands = read_stdin_commands(std::cin);
    std::vector<Request> requests;
    const ReqPackConfigOverrides inheritedOverrides = extract_cli_config_overrides(inheritedArguments);
    if (inheritedOverrides.errorMessage.has_value()) {
        logger.diagnostic(config_override_diagnostic(inheritedOverrides.errorMessage.value()));
        return 1;
    }
    ReqPackConfig effectiveConfig = apply_config_overrides(config, inheritedOverrides);

    for (const StdinCommand& command : commands) {
        const std::vector<std::string> commandTokens = tokenize_command_line(command.text);
        if (commandTokens.empty()) {
            logger.diagnostic(stdin_syntax_diagnostic(command.lineNumber, command.text));
            return 1;
        }

        const std::vector<std::string> actionTokens = action_stdin_payload_tokens(commandTokens, actionName);
        const std::vector<std::string> mergedTokens = merged_stream_command_arguments(actionTokens, inheritedArguments);
        const ReqPackConfigOverrides mergedOverrides = extract_cli_config_overrides(mergedTokens);
        if (mergedOverrides.errorMessage.has_value()) {
            logger.diagnostic(config_override_diagnostic("stdin line " + std::to_string(command.lineNumber) + ": " + mergedOverrides.errorMessage.value()));
            return 1;
        }
        effectiveConfig = apply_config_overrides(effectiveConfig, mergedOverrides);
        const std::vector<Request> parsed = cli.parse(mergedTokens, effectiveConfig);
        if (parsed.empty()) {
            if (!cli.lastParseError().empty()) {
                logger.diagnostic(config_override_diagnostic("stdin line " + std::to_string(command.lineNumber) + ": " + cli.lastParseError()));
                return 1;
            }
            logger.diagnostic(stdin_parse_diagnostic(command.lineNumber, command.text));
            return 1;
        }

        for (const Request& request : parsed) {
            if (request.action != expectedAction) {
                logger.diagnostic(stdin_action_only_diagnostic(command.lineNumber, actionName));
                return 1;
            }
            requests.push_back(request);
        }
    }

    if (requests.empty()) {
        logger.diagnostic(stdin_empty_batch_diagnostic(actionName));
        return 1;
    }

    Orchestrator orchestrator(std::move(requests), effectiveConfig);
    const int result = orchestrator.run();
    logger.flushSync();
    return result;
}

int run_stdin_serve_loop(Cli& cli,
                         const ReqPackConfig& config,
                         const std::vector<std::string>& inheritedArguments) {
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
            logger.diagnostic(stdin_syntax_diagnostic(lineNumber, trimmed));
            exitCode = 1;
            continue;
        }

        const std::vector<std::string> mergedTokens = merged_stream_command_arguments(commandTokens, inheritedArguments);
        const ReqPackConfigOverrides mergedOverrides = extract_cli_config_overrides(mergedTokens);
        if (mergedOverrides.errorMessage.has_value()) {
            logger.diagnostic(config_override_diagnostic("stdin line " + std::to_string(lineNumber) + ": " + mergedOverrides.errorMessage.value()));
            exitCode = 1;
            continue;
        }
        const ReqPackConfig effectiveConfig = apply_config_overrides(config, mergedOverrides);
        const std::vector<Request> requests = cli.parse(mergedTokens, effectiveConfig);
        if (requests.empty()) {
            if (!cli.lastParseError().empty()) {
                logger.diagnostic(config_override_diagnostic("stdin line " + std::to_string(lineNumber) + ": " + cli.lastParseError()));
                exitCode = 1;
                continue;
            }
            logger.diagnostic(stdin_parse_diagnostic(lineNumber, trimmed));
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
