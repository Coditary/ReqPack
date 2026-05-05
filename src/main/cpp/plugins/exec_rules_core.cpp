#include "plugins/exec_rules_core.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

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

ExecRuleActionType parse_action_type(const std::string& name) {
    const std::string normalized = to_lower_copy(name);
    if (normalized == "send") {
        return ExecRuleActionType::Send;
    }
    if (normalized == "state") {
        return ExecRuleActionType::State;
    }
    if (normalized == "log") {
        return ExecRuleActionType::Log;
    }
    if (normalized == "status") {
        return ExecRuleActionType::Status;
    }
    if (normalized == "progress") {
        return ExecRuleActionType::Progress;
    }
    if (normalized == "begin_step") {
        return ExecRuleActionType::BeginStep;
    }
    if (normalized == "success") {
        return ExecRuleActionType::Success;
    }
    if (normalized == "failed") {
        return ExecRuleActionType::Failed;
    }
    if (normalized == "event") {
        return ExecRuleActionType::Event;
    }
    if (normalized == "artifact") {
        return ExecRuleActionType::Artifact;
    }
    throw std::runtime_error("action type '" + name + "' is unknown.");
}

bool has_required_field(const ExecRuleAction& action, const std::string& name) {
    return action.fields.find(name) != action.fields.end();
}

void validate_action_fields(const ExecRuleAction& action) {
    switch (action.type) {
        case ExecRuleActionType::Send:
        case ExecRuleActionType::State:
            if (!has_required_field(action, "value")) {
                throw std::runtime_error("action requires field 'value'.");
            }
            break;
        case ExecRuleActionType::Log:
            if (!has_required_field(action, "message")) {
                throw std::runtime_error("log action requires field 'message'.");
            }
            break;
        case ExecRuleActionType::Status:
            if (!has_required_field(action, "code")) {
                throw std::runtime_error("status action requires field 'code'.");
            }
            break;
        case ExecRuleActionType::Progress:
            if (!has_required_field(action, "percent") && !has_required_field(action, "current") &&
                !has_required_field(action, "total") && !has_required_field(action, "speed")) {
                throw std::runtime_error("progress action requires field 'percent', 'current', 'total', or 'speed'.");
            }
            break;
        case ExecRuleActionType::BeginStep:
            if (!has_required_field(action, "label")) {
                throw std::runtime_error("begin_step action requires field 'label'.");
            }
            break;
        case ExecRuleActionType::Failed:
            if (!has_required_field(action, "message")) {
                throw std::runtime_error("failed action requires field 'message'.");
            }
            break;
        case ExecRuleActionType::Event:
            if (!has_required_field(action, "name")) {
                throw std::runtime_error("event action requires field 'name'.");
            }
            break;
        case ExecRuleActionType::Artifact:
            if (!has_required_field(action, "payload")) {
                throw std::runtime_error("artifact action requires field 'payload'.");
            }
            break;
        case ExecRuleActionType::Success:
            break;
    }
}

ExecRuleAction parse_action(const sol::object& object, std::size_t ruleIndex, std::size_t actionIndex) {
    if (!object.valid() || object.get_type() != sol::type::table) {
        throw std::runtime_error("rule[" + std::to_string(ruleIndex) + "].actions[" + std::to_string(actionIndex) + "] must be a table.");
    }

    const sol::table table = object.as<sol::table>();
    ExecRuleAction action;
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

        if (!value.valid() || value.is<sol::lua_nil_t>()) {
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

ExecRule parse_rule(const sol::object& object, std::size_t ruleIndex, bool& requiresPty) {
    if (!object.valid() || object.get_type() != sol::type::table) {
        throw std::runtime_error("rules.rules[" + std::to_string(ruleIndex) + "] must be a table.");
    }

    const sol::table table = object.as<sol::table>();
    ExecRule rule;
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
                rule.source = ExecRuleSource::Line;
            } else if (source == "screen") {
                rule.source = ExecRuleSource::Screen;
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
                ExecRuleAction action = parse_action(actionObjects[actionIndex], ruleIndex, actionIndex + 1);
                if (action.type == ExecRuleActionType::Send) {
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

std::string resolve_field(const ExecRuleAction& action, const std::string& name, const std::match_results<std::string::const_iterator>& match, const std::string& fallback = {}) {
    const auto it = action.fields.find(name);
    if (it == action.fields.end()) {
        return fallback;
    }
    return substitute_placeholders(it->second, match);
}

bool rule_active(const ExecRule& rule, const ExecRuleRuntimeState& runtime, std::size_t ruleIndex) {
    if (runtime.disabled[ruleIndex]) {
        return false;
    }
    return !rule.state.has_value() || rule.state.value() == runtime.currentState;
}

ResolvedExecRuleAction resolve_action(const ExecRuleAction& action, const std::match_results<std::string::const_iterator>& match) {
    ResolvedExecRuleAction resolved;
    resolved.type = action.type;
    for (const auto& [key, value] : action.fields) {
        resolved.fields[key] = substitute_placeholders(value, match);
    }
    return resolved;
}

ExecRuleEvaluationResult evaluate_rules_for_source(
    ExecRuleSource source,
    const ExecRuleset& ruleset,
    ExecRuleRuntimeState& runtime,
    const std::function<bool(std::size_t, const ExecRule&, std::match_results<std::string::const_iterator>&)>& tryMatch
) {
    ExecRuleEvaluationResult result;

    for (std::size_t ruleIndex = 0; ruleIndex < ruleset.rules.size(); ++ruleIndex) {
        const ExecRule& rule = ruleset.rules[ruleIndex];
        if (rule.source != source || !rule_active(rule, runtime, ruleIndex)) {
            continue;
        }

        std::match_results<std::string::const_iterator> match;
        if (!tryMatch(ruleIndex, rule, match)) {
            continue;
        }

        for (const ExecRuleAction& action : rule.actions) {
            const ResolvedExecRuleAction resolved = resolve_action(action, match);
            if (resolved.type == ExecRuleActionType::State) {
                const auto it = resolved.fields.find("value");
                if (it != resolved.fields.end() && !it->second.empty()) {
                    runtime.currentState = it->second;
                }
            }
            result.actions.push_back(resolved);
        }

        if (!rule.repeat) {
            runtime.disabled[ruleIndex] = true;
        }
        if (rule.stop) {
            result.stopTriggered = true;
            break;
        }
    }

    return result;
}

}  // namespace

ExecRuleset parse_exec_rules(const sol::object& rulesObject) {
    if (!rulesObject.valid() || rulesObject.get_type() != sol::type::table) {
        throw std::runtime_error("rules must be a table.");
    }

    const sol::table table = rulesObject.as<sol::table>();
    ExecRuleset ruleset;
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

ExecRuleRunnerMode determine_exec_rule_runner_mode(const ExecRuleset& ruleset) {
    if (ruleset.rules.empty()) {
        return ExecRuleRunnerMode::Plain;
    }
    if (ruleset.requiresPty) {
        return ExecRuleRunnerMode::Pty;
    }
    return ExecRuleRunnerMode::Line;
}

std::string normalize_exec_rule_pty_chunk(const std::string& chunk) {
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

ExecRuleRuntimeState make_exec_rule_runtime_state(const ExecRuleset& ruleset) {
    return ExecRuleRuntimeState{
        .currentState = ruleset.initialState,
        .disabled = std::vector<bool>(ruleset.rules.size(), false),
        .screenCursor = std::vector<std::size_t>(ruleset.rules.size(), 0)
    };
}

ExecRuleEvaluationResult evaluate_exec_rule_line_input(
    const ExecRuleset& ruleset,
    ExecRuleRuntimeState& runtime,
    const std::string& line
) {
    return evaluate_rules_for_source(ExecRuleSource::Line, ruleset, runtime, [&](std::size_t, const ExecRule& rule, std::match_results<std::string::const_iterator>& match) {
        return std::regex_search(line.begin(), line.end(), match, rule.regex);
    });
}

ExecRuleEvaluationResult evaluate_exec_rule_screen_input(
    const ExecRuleset& ruleset,
    ExecRuleRuntimeState& runtime,
    const std::string& transcript
) {
    return evaluate_rules_for_source(ExecRuleSource::Screen, ruleset, runtime, [&](std::size_t ruleIndex, const ExecRule& rule, std::match_results<std::string::const_iterator>& match) {
        const std::size_t cursor = std::min(runtime.screenCursor[ruleIndex], transcript.size());
        const auto begin = transcript.cbegin() + static_cast<std::ptrdiff_t>(cursor);
        if (!std::regex_search(begin, transcript.cend(), match, rule.regex)) {
            return false;
        }
        const std::size_t consumed = static_cast<std::size_t>(match.position()) + static_cast<std::size_t>(match.length());
        runtime.screenCursor[ruleIndex] = cursor + consumed;
        return true;
    });
}
