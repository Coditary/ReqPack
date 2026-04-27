#include <regex>

#include <catch2/catch.hpp>

#include "plugins/exec_rules_core.h"

namespace {

ExecRuleset make_control_ruleset() {
    ExecRuleset ruleset;
    ruleset.initialState = "default";

    ExecRule first;
    first.state = "default";
    first.source = ExecRuleSource::Line;
    first.regexText = "^TOKEN$";
    first.regex = std::regex(first.regexText, std::regex::ECMAScript);
    first.repeat = false;
    first.stop = true;
    first.actions = {
        ExecRuleAction{.type = ExecRuleActionType::Event, .fields = {{"name", "first"}, {"payload", "one"}}},
        ExecRuleAction{.type = ExecRuleActionType::State, .fields = {{"value", "done"}}},
    };

    ExecRule second;
    second.state = "default";
    second.source = ExecRuleSource::Line;
    second.regexText = "^TOKEN$";
    second.regex = std::regex(second.regexText, std::regex::ECMAScript);
    second.actions = {
        ExecRuleAction{.type = ExecRuleActionType::Event, .fields = {{"name", "second"}, {"payload", "two"}}},
    };

    ruleset.rules = {first, second};
    return ruleset;
}

ExecRuleset make_screen_ruleset() {
    ExecRuleset ruleset;
    ruleset.initialState = "confirm";
    ruleset.requiresPty = true;

    ExecRule screen;
    screen.state = "confirm";
    screen.source = ExecRuleSource::Screen;
    screen.regexText = "Continue\\? \\[[A-Za-z]/[A-Za-z]\\]:\\s*$";
    screen.regex = std::regex(screen.regexText, std::regex::ECMAScript);
    screen.repeat = false;
    screen.stop = true;
    screen.actions = {
        ExecRuleAction{.type = ExecRuleActionType::Send, .fields = {{"value", "y\n"}}},
        ExecRuleAction{.type = ExecRuleActionType::State, .fields = {{"value", "running"}}},
    };

    ExecRule line;
    line.state = "running";
    line.source = ExecRuleSource::Line;
    line.regexText = "^ACK=(.+)$";
    line.regex = std::regex(line.regexText, std::regex::ECMAScript);
    line.repeat = false;
    line.actions = {
        ExecRuleAction{.type = ExecRuleActionType::Event, .fields = {{"name", "ack"}, {"payload", "${1}"}}},
    };

    ruleset.rules = {screen, line};
    return ruleset;
}

}  // namespace

TEST_CASE("line evaluation applies stop, repeat, and state transitions", "[unit][exec_rules][evaluation]") {
    const ExecRuleset ruleset = make_control_ruleset();
    ExecRuleRuntimeState runtime = make_exec_rule_runtime_state(ruleset);

    const ExecRuleEvaluationResult first = evaluate_exec_rule_line_input(ruleset, runtime, "TOKEN");
    REQUIRE(first.actions.size() == 2);
    CHECK(first.actions[0].type == ExecRuleActionType::Event);
    CHECK(first.actions[0].fields.at("name") == "first");
    CHECK(first.stopTriggered);
    CHECK(runtime.currentState == "done");
    CHECK(runtime.disabled[0]);

    const ExecRuleEvaluationResult second = evaluate_exec_rule_line_input(ruleset, runtime, "TOKEN");
    CHECK(second.actions.empty());
}

TEST_CASE("evaluation resolves placeholders", "[unit][exec_rules][evaluation]") {
    ExecRuleset ruleset;
    ExecRule rule;
    rule.source = ExecRuleSource::Line;
    rule.regexText = "^Progress:\\s+(\\d+)%$";
    rule.regex = std::regex(rule.regexText, std::regex::ECMAScript);
    rule.actions = {
        ExecRuleAction{.type = ExecRuleActionType::Progress, .fields = {{"percent", "${1}"}}},
        ExecRuleAction{.type = ExecRuleActionType::Event, .fields = {{"name", "progress"}, {"payload", "match=${0};value=${1}"}}},
    };
    ruleset.rules = {rule};

    ExecRuleRuntimeState runtime = make_exec_rule_runtime_state(ruleset);
    const ExecRuleEvaluationResult result = evaluate_exec_rule_line_input(ruleset, runtime, "Progress: 42%");

    REQUIRE(result.actions.size() == 2);
    CHECK(result.actions[0].fields.at("percent") == "42");
    CHECK(result.actions[1].fields.at("payload") == "match=Progress: 42%;value=42");
}

TEST_CASE("screen evaluation advances cursor and enables later line rules", "[unit][exec_rules][evaluation]") {
    const ExecRuleset ruleset = make_screen_ruleset();
    ExecRuleRuntimeState runtime = make_exec_rule_runtime_state(ruleset);

    const ExecRuleEvaluationResult screenFirst = evaluate_exec_rule_screen_input(ruleset, runtime, "Continue? [y/N]: ");
    REQUIRE(screenFirst.actions.size() == 2);
    CHECK(screenFirst.actions[0].type == ExecRuleActionType::Send);
    CHECK(runtime.currentState == "running");
    CHECK(runtime.disabled[0]);

    const ExecRuleEvaluationResult screenSecond = evaluate_exec_rule_screen_input(ruleset, runtime, "Continue? [y/N]: ");
    CHECK(screenSecond.actions.empty());

    const ExecRuleEvaluationResult lineResult = evaluate_exec_rule_line_input(ruleset, runtime, "ACK=y");
    REQUIRE(lineResult.actions.size() == 1);
    CHECK(lineResult.actions[0].fields.at("payload") == "y");
}
