#pragma once

#include <cstddef>
#include <optional>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

#include <sol/sol.hpp>

enum class ExecRuleSource {
    Line,
    Screen
};

enum class ExecRuleRunnerMode {
    Plain,
    Line,
    Pty
};

enum class ExecRuleActionType {
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

struct ExecRuleAction {
    ExecRuleActionType type{ExecRuleActionType::Log};
    std::unordered_map<std::string, std::string> fields{};
};

struct ExecRule {
    std::optional<std::string> state{};
    ExecRuleSource source{ExecRuleSource::Line};
    std::string regexText{};
    std::regex regex{};
    std::vector<ExecRuleAction> actions{};
    bool repeat{true};
    bool stop{false};
};

struct ExecRuleset {
    std::string initialState{"default"};
    std::vector<ExecRule> rules{};
    bool requiresPty{false};
};

struct ExecRuleRuntimeState {
    std::string currentState{};
    std::vector<bool> disabled{};
    std::vector<std::size_t> screenCursor{};
};

struct ResolvedExecRuleAction {
    ExecRuleActionType type{ExecRuleActionType::Log};
    std::unordered_map<std::string, std::string> fields{};
};

struct ExecRuleEvaluationResult {
    std::vector<ResolvedExecRuleAction> actions{};
    bool stopTriggered{false};
};

ExecRuleset parse_exec_rules(const sol::object& rulesObject);
ExecRuleRunnerMode determine_exec_rule_runner_mode(const ExecRuleset& ruleset);
std::string normalize_exec_rule_pty_chunk(const std::string& chunk);
ExecRuleRuntimeState make_exec_rule_runtime_state(const ExecRuleset& ruleset);
ExecRuleEvaluationResult evaluate_exec_rule_line_input(
    const ExecRuleset& ruleset,
    ExecRuleRuntimeState& runtime,
    const std::string& line
);
ExecRuleEvaluationResult evaluate_exec_rule_screen_input(
    const ExecRuleset& ruleset,
    ExecRuleRuntimeState& runtime,
    const std::string& transcript
);
