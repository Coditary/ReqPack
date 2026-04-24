#include "plugins/exec_rules.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <pty.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

enum class RuleSource {
    Line,
    Screen
};

enum class RunnerMode {
    Plain,
    Line,
    Pty
};

enum class RuleActionType {
    Send,
    State,
    Log,
    Status,
    Progress,
    BeginStep,
    Success,
    Failed,
    Event,
    Artifact
};

struct ParsedRuleAction {
    RuleActionType type{RuleActionType::Log};
    std::unordered_map<std::string, std::string> fields;
};

struct ParsedRule {
    std::optional<std::string> state;
    RuleSource source{RuleSource::Line};
    std::string regexText;
    std::regex regex;
    std::vector<ParsedRuleAction> actions;
    bool repeat{true};
    bool stop{false};
};

struct ParsedRuleset {
    std::string initialState{"default"};
    std::vector<ParsedRule> rules;
    bool requiresPty{false};
};

struct RuleRuntimeState {
    std::string currentState;
    std::vector<bool> disabled;
    std::vector<std::size_t> screenCursor;
};

struct ActionExecutionContext {
    Logger& logger;
    const std::string& pluginId;
    RuleRuntimeState& runtime;
    std::optional<int> masterFd;
};

struct LineAccumulator {
    std::string buffer;

    template <typename Callback>
    bool append(const std::string& text, Callback&& callback) {
        bool stop = false;
        std::size_t start = 0;
        while (start < text.size()) {
            const std::size_t newline = text.find('\n', start);
            if (newline == std::string::npos) {
                buffer.append(text.substr(start));
                break;
            }

            buffer.append(text.substr(start, newline - start));
            stop = callback(buffer) || stop;
            buffer.clear();
            start = newline + 1;
        }
        return stop;
    }

    template <typename Callback>
    bool flush(Callback&& callback) {
        if (buffer.empty()) {
            return false;
        }
        const std::string line = buffer;
        buffer.clear();
        return callback(line);
    }
};

std::string to_lower_copy(const std::string& value) {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return normalized;
}

std::string scalar_to_string(const sol::object& value) {
    if (!value.valid()) {
        return {};
    }
    if (value.is<std::string>()) {
        return value.as<std::string>();
    }
    if (value.is<bool>()) {
        return value.as<bool>() ? "true" : "false";
    }
    if (value.is<int>()) {
        return std::to_string(value.as<int>());
    }
    if (value.is<long long>()) {
        return std::to_string(value.as<long long>());
    }
    if (value.is<double>()) {
        return std::to_string(value.as<double>());
    }
    return {};
}

void log_plugin_message(Logger& logger, spdlog::level::level_enum level, const std::string& pluginId, const std::string& message) {
    logger.emit(OutputAction::LOG, OutputContext{.level = level, .message = message, .source = "plugin", .scope = pluginId});
}

void log_rule_warning(Logger& logger, const std::string& pluginId, const std::string& message) {
    log_plugin_message(logger, spdlog::level::warn, pluginId, "exec-rule warning: " + message);
}

void log_rule_error(Logger& logger, const std::string& pluginId, const std::string& message) {
    log_plugin_message(logger, spdlog::level::err, pluginId, "exec-rule error: " + message);
}

std::string escape_shell_double_quotes(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value) {
        if (c == '\\' || c == '"' || c == '$' || c == '`') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    return escaped;
}

std::optional<int> numeric_key_to_index(const sol::object& key) {
    if (key.is<int>()) {
        return key.as<int>();
    }
    if (key.is<double>()) {
        const double raw = key.as<double>();
        const int integer = static_cast<int>(raw);
        if (raw == static_cast<double>(integer)) {
            return integer;
        }
    }
    return std::nullopt;
}

std::vector<sol::object> read_array_table(const sol::table& table, const std::string& context) {
    std::vector<std::pair<int, sol::object>> indexed;
    indexed.reserve(table.size());

    for (const auto& [key, value] : table) {
        const std::optional<int> index = numeric_key_to_index(key);
        if (!index.has_value() || index.value() < 1) {
            throw std::runtime_error(context + " must be an array-style table.");
        }
        indexed.emplace_back(index.value(), value);
    }

    std::sort(indexed.begin(), indexed.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });

    std::vector<sol::object> result;
    result.reserve(indexed.size());

    int expected = 1;
    for (const auto& [index, value] : indexed) {
        if (index != expected) {
            throw std::runtime_error(context + " must be contiguous and start at index 1.");
        }
        result.push_back(value);
        ++expected;
    }

    return result;
}

RuleActionType parse_action_type(const std::string& name) {
    const std::string normalized = to_lower_copy(name);
    if (normalized == "send") {
        return RuleActionType::Send;
    }
    if (normalized == "state") {
        return RuleActionType::State;
    }
    if (normalized == "log") {
        return RuleActionType::Log;
    }
    if (normalized == "status") {
        return RuleActionType::Status;
    }
    if (normalized == "progress") {
        return RuleActionType::Progress;
    }
    if (normalized == "begin_step") {
        return RuleActionType::BeginStep;
    }
    if (normalized == "success") {
        return RuleActionType::Success;
    }
    if (normalized == "failed") {
        return RuleActionType::Failed;
    }
    if (normalized == "event") {
        return RuleActionType::Event;
    }
    if (normalized == "artifact") {
        return RuleActionType::Artifact;
    }
    throw std::runtime_error("action type '" + name + "' is unknown.");
}

bool has_required_field(const ParsedRuleAction& action, const std::string& name) {
    return action.fields.find(name) != action.fields.end();
}

void validate_action_fields(const ParsedRuleAction& action) {
    switch (action.type) {
        case RuleActionType::Send:
        case RuleActionType::State:
            if (!has_required_field(action, "value")) {
                throw std::runtime_error("action requires field 'value'.");
            }
            break;
        case RuleActionType::Log:
            if (!has_required_field(action, "message")) {
                throw std::runtime_error("log action requires field 'message'.");
            }
            break;
        case RuleActionType::Status:
            if (!has_required_field(action, "code")) {
                throw std::runtime_error("status action requires field 'code'.");
            }
            break;
        case RuleActionType::Progress:
            if (!has_required_field(action, "percent")) {
                throw std::runtime_error("progress action requires field 'percent'.");
            }
            break;
        case RuleActionType::BeginStep:
            if (!has_required_field(action, "label")) {
                throw std::runtime_error("begin_step action requires field 'label'.");
            }
            break;
        case RuleActionType::Failed:
            if (!has_required_field(action, "message")) {
                throw std::runtime_error("failed action requires field 'message'.");
            }
            break;
        case RuleActionType::Event:
            if (!has_required_field(action, "name")) {
                throw std::runtime_error("event action requires field 'name'.");
            }
            break;
        case RuleActionType::Artifact:
            if (!has_required_field(action, "payload")) {
                throw std::runtime_error("artifact action requires field 'payload'.");
            }
            break;
        case RuleActionType::Success:
            break;
    }
}

ParsedRuleAction parse_action(const sol::object& object, std::size_t ruleIndex, std::size_t actionIndex) {
    if (!object.valid() || object.get_type() != sol::type::table) {
        throw std::runtime_error("rule[" + std::to_string(ruleIndex) + "].actions[" + std::to_string(actionIndex) + "] must be a table.");
    }

    const sol::table table = object.as<sol::table>();
    ParsedRuleAction action;
    bool hasType = false;

    for (const auto& [key, value] : table) {
        if (!key.is<std::string>()) {
            throw std::runtime_error("rule[" + std::to_string(ruleIndex) + "].actions[" + std::to_string(actionIndex) + "] keys must be strings.");
        }

        const std::string name = key.as<std::string>();
        if (name == "type") {
            if (!value.is<std::string>()) {
                throw std::runtime_error("rule[" + std::to_string(ruleIndex) + "].actions[" + std::to_string(actionIndex) + "].type must be a string.");
            }
            action.type = parse_action_type(value.as<std::string>());
            hasType = true;
            continue;
        }

        if (!value.valid() || value.get_type() == sol::type::nil) {
            continue;
        }

        if (value.get_type() == sol::type::table || value.get_type() == sol::type::userdata || value.get_type() == sol::type::function || value.get_type() == sol::type::thread || value.get_type() == sol::type::lightuserdata) {
            throw std::runtime_error("rule[" + std::to_string(ruleIndex) + "].actions[" + std::to_string(actionIndex) + "] contains unsupported nested value for field '" + name + "'.");
        }

        action.fields[name] = scalar_to_string(value);
    }

    if (!hasType) {
        throw std::runtime_error("rule[" + std::to_string(ruleIndex) + "].actions[" + std::to_string(actionIndex) + "] is missing field 'type'.");
    }

    try {
        validate_action_fields(action);
    } catch (const std::exception& error) {
        throw std::runtime_error("rule[" + std::to_string(ruleIndex) + "].actions[" + std::to_string(actionIndex) + "]: " + error.what());
    }

    return action;
}

ParsedRule parse_rule(const sol::object& object, std::size_t ruleIndex, bool& requiresPty) {
    if (!object.valid() || object.get_type() != sol::type::table) {
        throw std::runtime_error("rules.rules[" + std::to_string(ruleIndex) + "] must be a table.");
    }

    const sol::table table = object.as<sol::table>();
    ParsedRule rule;
    bool hasSource = false;
    bool hasRegex = false;
    bool hasActions = false;

    for (const auto& [key, value] : table) {
        if (!key.is<std::string>()) {
            throw std::runtime_error("rules.rules[" + std::to_string(ruleIndex) + "] keys must be strings.");
        }

        const std::string name = key.as<std::string>();
        if (name == "state") {
            if (!value.is<std::string>()) {
                throw std::runtime_error("rules.rules[" + std::to_string(ruleIndex) + "].state must be a string.");
            }
            rule.state = value.as<std::string>();
            continue;
        }

        if (name == "source") {
            if (!value.is<std::string>()) {
                throw std::runtime_error("rules.rules[" + std::to_string(ruleIndex) + "].source must be a string.");
            }
            const std::string source = to_lower_copy(value.as<std::string>());
            if (source == "line") {
                rule.source = RuleSource::Line;
            } else if (source == "screen") {
                rule.source = RuleSource::Screen;
                requiresPty = true;
            } else {
                throw std::runtime_error("rules.rules[" + std::to_string(ruleIndex) + "].source must be 'line' or 'screen'.");
            }
            hasSource = true;
            continue;
        }

        if (name == "regex") {
            if (!value.is<std::string>()) {
                throw std::runtime_error("rules.rules[" + std::to_string(ruleIndex) + "].regex must be a string.");
            }
            rule.regexText = value.as<std::string>();
            try {
                rule.regex = std::regex(rule.regexText, std::regex::ECMAScript);
            } catch (const std::regex_error& error) {
                throw std::runtime_error("rules.rules[" + std::to_string(ruleIndex) + "].regex failed to compile: " + std::string(error.what()));
            }
            hasRegex = true;
            continue;
        }

        if (name == "actions") {
            if (!value.valid() || value.get_type() != sol::type::table) {
                throw std::runtime_error("rules.rules[" + std::to_string(ruleIndex) + "].actions must be an array-style table.");
            }
            const std::vector<sol::object> actionObjects = read_array_table(value.as<sol::table>(), "rules.rules[" + std::to_string(ruleIndex) + "].actions");
            if (actionObjects.empty()) {
                throw std::runtime_error("rules.rules[" + std::to_string(ruleIndex) + "].actions must not be empty.");
            }
            rule.actions.reserve(actionObjects.size());
            for (std::size_t actionIndex = 0; actionIndex < actionObjects.size(); ++actionIndex) {
                ParsedRuleAction action = parse_action(actionObjects[actionIndex], ruleIndex, actionIndex + 1);
                if (action.type == RuleActionType::Send) {
                    requiresPty = true;
                }
                rule.actions.push_back(std::move(action));
            }
            hasActions = true;
            continue;
        }

        if (name == "repeat") {
            if (!value.is<bool>()) {
                throw std::runtime_error("rules.rules[" + std::to_string(ruleIndex) + "].repeat must be a boolean.");
            }
            rule.repeat = value.as<bool>();
            continue;
        }

        if (name == "stop") {
            if (!value.is<bool>()) {
                throw std::runtime_error("rules.rules[" + std::to_string(ruleIndex) + "].stop must be a boolean.");
            }
            rule.stop = value.as<bool>();
            continue;
        }

        throw std::runtime_error("rules.rules[" + std::to_string(ruleIndex) + "] contains unknown key '" + name + "'.");
    }

    if (!hasSource) {
        throw std::runtime_error("rules.rules[" + std::to_string(ruleIndex) + "] is missing field 'source'.");
    }
    if (!hasRegex) {
        throw std::runtime_error("rules.rules[" + std::to_string(ruleIndex) + "] is missing field 'regex'.");
    }
    if (!hasActions) {
        throw std::runtime_error("rules.rules[" + std::to_string(ruleIndex) + "] is missing field 'actions'.");
    }

    return rule;
}

ParsedRuleset parse_rules(const sol::object& rulesObject) {
    if (!rulesObject.valid() || rulesObject.get_type() != sol::type::table) {
        throw std::runtime_error("rules must be a table.");
    }

    const sol::table table = rulesObject.as<sol::table>();
    ParsedRuleset ruleset;
    std::optional<sol::object> ruleArrayObject;

    for (const auto& [key, value] : table) {
        if (!key.is<std::string>()) {
            throw std::runtime_error("rules top-level keys must be strings.");
        }

        const std::string name = key.as<std::string>();
        if (name == "initial") {
            if (!value.is<std::string>()) {
                throw std::runtime_error("rules.initial must be a string.");
            }
            ruleset.initialState = value.as<std::string>();
            continue;
        }

        if (name == "rules") {
            ruleArrayObject = value;
            continue;
        }

        throw std::runtime_error("rules contains unknown top-level key '" + name + "'.");
    }

    if (!ruleArrayObject.has_value() || !ruleArrayObject->valid() || ruleArrayObject->get_type() != sol::type::table) {
        throw std::runtime_error("rules.rules must be an array-style table.");
    }

    const std::vector<sol::object> ruleObjects = read_array_table(ruleArrayObject->as<sol::table>(), "rules.rules");
    ruleset.rules.reserve(ruleObjects.size());
    for (std::size_t index = 0; index < ruleObjects.size(); ++index) {
        ruleset.rules.push_back(parse_rule(ruleObjects[index], index + 1, ruleset.requiresPty));
    }

    return ruleset;
}

std::string substitute_placeholders(const std::string& input, const std::match_results<std::string::const_iterator>& match) {
    std::string result;
    result.reserve(input.size());

    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] == '$' && index + 2 < input.size() && input[index + 1] == '{') {
            const std::size_t end = input.find('}', index + 2);
            if (end != std::string::npos) {
                const std::string token = input.substr(index + 2, end - (index + 2));
                try {
                    const std::size_t captureIndex = static_cast<std::size_t>(std::stoul(token));
                    if (captureIndex < match.size()) {
                        result.append(match[captureIndex].first, match[captureIndex].second);
                    }
                    index = end;
                    continue;
                } catch (const std::exception&) {
                }
            }
        }
        result.push_back(input[index]);
    }

    return result;
}

std::string resolve_field(const ParsedRuleAction& action, const std::string& name, const std::match_results<std::string::const_iterator>& match, const std::string& fallback = {}) {
    const auto it = action.fields.find(name);
    if (it == action.fields.end()) {
        return fallback;
    }
    return substitute_placeholders(it->second, match);
}

std::optional<int> parse_int_value(const std::string& value) {
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(value, &consumed);
        if (consumed != value.size()) {
            return std::nullopt;
        }
        return parsed;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

spdlog::level::level_enum parse_log_level(const std::string& value) {
    const std::string normalized = to_lower_copy(value);
    if (normalized.empty() || normalized == "info") {
        return spdlog::level::info;
    }
    if (normalized == "debug") {
        return spdlog::level::debug;
    }
    if (normalized == "warn") {
        return spdlog::level::warn;
    }
    if (normalized == "error") {
        return spdlog::level::err;
    }
    throw std::runtime_error("invalid log level '" + value + "'.");
}

bool write_all(int fd, const std::string& value) {
    std::size_t written = 0;
    while (written < value.size()) {
        const ssize_t count = ::write(fd, value.data() + written, value.size() - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    return true;
}

void emit_log_action(Logger& logger, const std::string& pluginId, spdlog::level::level_enum level, const std::string& message) {
    logger.emit(OutputAction::LOG, OutputContext{.level = level, .message = message, .source = "plugin", .scope = pluginId});
}

void emit_status_action(Logger& logger, const std::string& pluginId, int statusCode) {
    logger.emit(OutputAction::PLUGIN_STATUS, OutputContext{.source = "plugin", .scope = pluginId, .statusCode = statusCode});
}

void emit_progress_action(Logger& logger, const std::string& pluginId, int percent) {
    logger.emit(OutputAction::PLUGIN_PROGRESS, OutputContext{.source = "plugin", .scope = pluginId, .progressPercent = std::clamp(percent, 0, 100)});
}

void emit_event_action(Logger& logger, const std::string& pluginId, const std::string& name, const std::string& payload) {
    logger.emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = "plugin", .scope = pluginId, .eventName = name, .payload = payload});
}

void emit_artifact_action(Logger& logger, const std::string& pluginId, const std::string& payload) {
    logger.emit(OutputAction::PLUGIN_ARTIFACT, OutputContext{.source = "plugin", .scope = pluginId, .payload = payload});
}

void execute_action(const ParsedRuleAction& action, const std::match_results<std::string::const_iterator>& match, ActionExecutionContext& context) {
    try {
        switch (action.type) {
            case RuleActionType::Send: {
                if (!context.masterFd.has_value()) {
                    log_rule_warning(context.logger, context.pluginId, "send action skipped because PTY writer is unavailable.");
                    return;
                }
                const std::string value = resolve_field(action, "value", match);
                if (value.empty()) {
                    log_rule_warning(context.logger, context.pluginId, "send action resolved to empty value.");
                    return;
                }
                if (!write_all(context.masterFd.value(), value)) {
                    log_rule_warning(context.logger, context.pluginId, "send action failed to write to child process.");
                }
                return;
            }
            case RuleActionType::State: {
                const std::string value = resolve_field(action, "value", match);
                if (value.empty()) {
                    log_rule_warning(context.logger, context.pluginId, "state action resolved to empty value.");
                    return;
                }
                context.runtime.currentState = value;
                return;
            }
            case RuleActionType::Log: {
                const std::string level = resolve_field(action, "level", match, "info");
                const std::string message = resolve_field(action, "message", match);
                emit_log_action(context.logger, context.pluginId, parse_log_level(level), message);
                return;
            }
            case RuleActionType::Status: {
                const std::string raw = resolve_field(action, "code", match);
                const std::optional<int> code = parse_int_value(raw);
                if (!code.has_value()) {
                    log_rule_warning(context.logger, context.pluginId, "status action value '" + raw + "' is not an integer.");
                    return;
                }
                emit_status_action(context.logger, context.pluginId, code.value());
                return;
            }
            case RuleActionType::Progress: {
                const std::string raw = resolve_field(action, "percent", match);
                const std::optional<int> percent = parse_int_value(raw);
                if (!percent.has_value()) {
                    log_rule_warning(context.logger, context.pluginId, "progress action value '" + raw + "' is not an integer.");
                    return;
                }
                emit_progress_action(context.logger, context.pluginId, percent.value());
                return;
            }
            case RuleActionType::BeginStep:
                emit_event_action(context.logger, context.pluginId, "begin_step", resolve_field(action, "label", match));
                return;
            case RuleActionType::Success:
                emit_event_action(context.logger, context.pluginId, "success", "ok");
                return;
            case RuleActionType::Failed:
                emit_event_action(context.logger, context.pluginId, "failed", resolve_field(action, "message", match));
                return;
            case RuleActionType::Event: {
                const std::string name = resolve_field(action, "name", match);
                if (name.empty()) {
                    log_rule_warning(context.logger, context.pluginId, "event action resolved to empty name.");
                    return;
                }
                emit_event_action(context.logger, context.pluginId, name, resolve_field(action, "payload", match));
                return;
            }
            case RuleActionType::Artifact:
                emit_artifact_action(context.logger, context.pluginId, resolve_field(action, "payload", match));
                return;
        }
    } catch (const std::exception& error) {
        log_rule_warning(context.logger, context.pluginId, error.what());
    }
}

bool rule_active(const ParsedRule& rule, const RuleRuntimeState& runtime, std::size_t ruleIndex) {
    if (runtime.disabled[ruleIndex]) {
        return false;
    }
    return !rule.state.has_value() || rule.state.value() == runtime.currentState;
}

bool evaluate_line_rules(const ParsedRuleset& ruleset, RuleRuntimeState& runtime, const std::string& line, ActionExecutionContext& actionContext) {
    for (std::size_t ruleIndex = 0; ruleIndex < ruleset.rules.size(); ++ruleIndex) {
        const ParsedRule& rule = ruleset.rules[ruleIndex];
        if (rule.source != RuleSource::Line || !rule_active(rule, runtime, ruleIndex)) {
            continue;
        }

        std::match_results<std::string::const_iterator> match;
        if (!std::regex_search(line.begin(), line.end(), match, rule.regex)) {
            continue;
        }

        for (const ParsedRuleAction& action : rule.actions) {
            execute_action(action, match, actionContext);
        }

        if (!rule.repeat) {
            runtime.disabled[ruleIndex] = true;
        }
        if (rule.stop) {
            return true;
        }
    }
    return false;
}

bool evaluate_screen_rules(const ParsedRuleset& ruleset, RuleRuntimeState& runtime, const std::string& transcript, ActionExecutionContext& actionContext) {
    for (std::size_t ruleIndex = 0; ruleIndex < ruleset.rules.size(); ++ruleIndex) {
        const ParsedRule& rule = ruleset.rules[ruleIndex];
        if (rule.source != RuleSource::Screen || !rule_active(rule, runtime, ruleIndex)) {
            continue;
        }

        const std::size_t cursor = std::min(runtime.screenCursor[ruleIndex], transcript.size());
        const auto begin = transcript.cbegin() + static_cast<std::ptrdiff_t>(cursor);
        std::match_results<std::string::const_iterator> match;
        if (!std::regex_search(begin, transcript.cend(), match, rule.regex)) {
            continue;
        }

        for (const ParsedRuleAction& action : rule.actions) {
            execute_action(action, match, actionContext);
        }

        const std::size_t consumed = static_cast<std::size_t>(match.position()) + static_cast<std::size_t>(match.length());
        runtime.screenCursor[ruleIndex] = cursor + consumed;
        if (!rule.repeat) {
            runtime.disabled[ruleIndex] = true;
        }
        if (rule.stop) {
            return true;
        }
    }
    return false;
}

RunnerMode determine_runner_mode(const ParsedRuleset& ruleset) {
    if (ruleset.rules.empty()) {
        return RunnerMode::Plain;
    }
    if (ruleset.requiresPty) {
        return RunnerMode::Pty;
    }
    return RunnerMode::Line;
}

std::string normalize_pty_chunk(const std::string& chunk) {
    std::string normalized;
    normalized.reserve(chunk.size());

    for (std::size_t index = 0; index < chunk.size();) {
        const unsigned char current = static_cast<unsigned char>(chunk[index]);
        if (current == '\x1b') {
            if (index + 1 < chunk.size() && chunk[index + 1] == '[') {
                index += 2;
                while (index < chunk.size()) {
                    const unsigned char c = static_cast<unsigned char>(chunk[index]);
                    if (c >= 0x40 && c <= 0x7e) {
                        ++index;
                        break;
                    }
                    ++index;
                }
                continue;
            }
            if (index + 1 < chunk.size() && chunk[index + 1] == ']') {
                index += 2;
                while (index < chunk.size()) {
                    if (chunk[index] == '\a') {
                        ++index;
                        break;
                    }
                    if (chunk[index] == '\x1b' && index + 1 < chunk.size() && chunk[index + 1] == '\\') {
                        index += 2;
                        break;
                    }
                    ++index;
                }
                continue;
            }
            index += std::min<std::size_t>(2, chunk.size() - index);
            continue;
        }

        if (chunk[index] == '\r') {
            normalized.push_back('\n');
            if (index + 1 < chunk.size() && chunk[index + 1] == '\n') {
                index += 2;
            } else {
                ++index;
            }
            continue;
        }

        if (chunk[index] == '\b') {
            if (!normalized.empty()) {
                normalized.pop_back();
            }
            ++index;
            continue;
        }

        if (current < 0x20 && chunk[index] != '\n' && chunk[index] != '\t') {
            ++index;
            continue;
        }

        normalized.push_back(chunk[index]);
        ++index;
    }

    return normalized;
}

ExecResult run_plain_command(Logger& logger, const std::string& pluginId, const std::string& command) {
    ExecResult result;
    const std::string wrappedCommand = "zsh -lc \"" + escape_shell_double_quotes(command) + "\" 2>&1";
    FILE* pipe = popen(wrappedCommand.c_str(), "r");
    if (pipe == nullptr) {
        result.stderrText = "failed to open process";
        return result;
    }

    std::array<char, 4096> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        const std::string chunk(buffer.data());
        result.stdoutText += chunk;
        logger.stdout(chunk, pluginId, "exec");
    }

    const int status = pclose(pipe);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : status;
    result.success = result.exitCode == 0;
    if (!result.success && result.stderrText.empty()) {
        result.stderrText = result.stdoutText;
    }
    return result;
}

ExecResult run_line_command(Logger& logger, const std::string& pluginId, const std::string& command, const ParsedRuleset& ruleset) {
    ExecResult result;
    const std::string wrappedCommand = "zsh -lc \"" + escape_shell_double_quotes(command) + "\" 2>&1";
    FILE* pipe = popen(wrappedCommand.c_str(), "r");
    if (pipe == nullptr) {
        result.stderrText = "failed to open process";
        return result;
    }

    RuleRuntimeState runtime{
        .currentState = ruleset.initialState,
        .disabled = std::vector<bool>(ruleset.rules.size(), false),
        .screenCursor = std::vector<std::size_t>(ruleset.rules.size(), 0)
    };
    ActionExecutionContext actionContext{.logger = logger, .pluginId = pluginId, .runtime = runtime, .masterFd = std::nullopt};
    LineAccumulator lines;

    std::array<char, 4096> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        const std::string chunk(buffer.data());
        result.stdoutText += chunk;
        logger.stdout(chunk, pluginId, "exec");
        lines.append(chunk, [&](const std::string& line) {
            return evaluate_line_rules(ruleset, runtime, line, actionContext);
        });
    }
    lines.flush([&](const std::string& line) {
        return evaluate_line_rules(ruleset, runtime, line, actionContext);
    });

    const int status = pclose(pipe);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : status;
    result.success = result.exitCode == 0;
    if (!result.success && result.stderrText.empty()) {
        result.stderrText = result.stdoutText;
    }
    return result;
}

ExecResult run_pty_command(Logger& logger, const std::string& pluginId, const std::string& command, const ParsedRuleset& ruleset) {
    ExecResult result;

    int masterFd = -1;
    const pid_t child = forkpty(&masterFd, nullptr, nullptr, nullptr);
    if (child < 0) {
        result.stderrText = std::string("failed to create PTY: ") + std::strerror(errno);
        return result;
    }

    if (child == 0) {
        execlp("zsh", "zsh", "-lc", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    RuleRuntimeState runtime{
        .currentState = ruleset.initialState,
        .disabled = std::vector<bool>(ruleset.rules.size(), false),
        .screenCursor = std::vector<std::size_t>(ruleset.rules.size(), 0)
    };
    ActionExecutionContext actionContext{.logger = logger, .pluginId = pluginId, .runtime = runtime, .masterFd = masterFd};
    LineAccumulator lines;
    std::string normalizedTranscript;

    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t count = ::read(masterFd, buffer.data(), buffer.size());
        if (count > 0) {
            const std::string chunk(buffer.data(), static_cast<std::size_t>(count));
            result.stdoutText += chunk;
            const std::string normalized = normalize_pty_chunk(chunk);
            if (!normalized.empty()) {
                normalizedTranscript += normalized;
                lines.append(normalized, [&](const std::string& line) {
                    return evaluate_line_rules(ruleset, runtime, line, actionContext);
                });
                evaluate_screen_rules(ruleset, runtime, normalizedTranscript, actionContext);
            }
            continue;
        }

        if (count == 0) {
            break;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EIO) {
            break;
        }

        result.stderrText = std::string("PTY read failed: ") + std::strerror(errno);
        break;
    }

    lines.flush([&](const std::string& line) {
        return evaluate_line_rules(ruleset, runtime, line, actionContext);
    });
    evaluate_screen_rules(ruleset, runtime, normalizedTranscript, actionContext);

    ::close(masterFd);

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            result.stderrText = std::string("waitpid failed: ") + std::strerror(errno);
            result.exitCode = 1;
            result.success = false;
            return result;
        }
    }

    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : status;
    result.success = result.stderrText.empty() && result.exitCode == 0;
    return result;
}

}  // namespace

ExecResult run_plugin_command(Logger& logger, const std::string& pluginId, const std::string& command) {
    return run_plain_command(logger, pluginId, command);
}

ExecResult run_plugin_command(Logger& logger, const std::string& pluginId, const std::string& command, const sol::object& rules) {
    ParsedRuleset ruleset;
    try {
        ruleset = parse_rules(rules);
    } catch (const std::exception& error) {
        log_rule_error(logger, pluginId, error.what());
        return ExecResult{.success = false, .exitCode = 1, .stdoutText = {}, .stderrText = error.what()};
    }

    switch (determine_runner_mode(ruleset)) {
        case RunnerMode::Plain:
            return run_plain_command(logger, pluginId, command);
        case RunnerMode::Line:
            return run_line_command(logger, pluginId, command, ruleset);
        case RunnerMode::Pty:
            return run_pty_command(logger, pluginId, command, ruleset);
    }

    return ExecResult{.success = false, .exitCode = 1, .stdoutText = {}, .stderrText = "unknown runner mode"};
}
