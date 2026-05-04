#include <sol/sol.hpp>

#include <stdexcept>
#include <string>

#include <catch2/catch.hpp>

#include "plugins/exec_rules_core.h"

namespace {

sol::object eval_lua(sol::state& lua, const std::string& source) {
    sol::load_result loaded = lua.load(source);
    REQUIRE(loaded.valid());
    sol::protected_function_result result = loaded();
    REQUIRE(result.valid());
    return result.get<sol::object>();
}

sol::state make_lua() {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::math);
    return lua;
}

}  // namespace

TEST_CASE("exec rules parser accepts valid unified rules", "[unit][exec_rules][parse]") {
    sol::state lua = make_lua();
    const sol::object rulesObject = eval_lua(lua, R"(
        return {
            initial = "confirm",
            rules = {
                {
                    state = "confirm",
                    source = "screen",
                    regex = "Continue\\?",
                    ["repeat"] = false,
                    stop = true,
                    actions = {
                        { type = "send", value = "y\n" },
                        { type = "state", value = "running" },
                    },
                },
                {
                    state = "running",
                    source = "line",
                    regex = "^Progress:\\s+(\\d+)%$",
                    actions = {
                        { type = "progress", percent = "${1}", current = "16.4", currentUnit = "MiB", total = "40.0", totalUnit = "MiB", speed = "2.5", speedUnit = "MiB/s" },
                    },
                },
            },
        }
    )");

    const ExecRuleset ruleset = parse_exec_rules(rulesObject);
    REQUIRE(ruleset.initialState == "confirm");
    REQUIRE(ruleset.rules.size() == 2);
    REQUIRE(ruleset.requiresPty);
    CHECK(ruleset.rules[0].source == ExecRuleSource::Screen);
    CHECK_FALSE(ruleset.rules[0].repeat);
    CHECK(ruleset.rules[0].stop);
    REQUIRE(ruleset.rules[0].actions.size() == 2);
    CHECK(ruleset.rules[0].actions[0].type == ExecRuleActionType::Send);
    CHECK(ruleset.rules[1].source == ExecRuleSource::Line);
}

TEST_CASE("exec rules parser rejects invalid source", "[unit][exec_rules][parse]") {
    sol::state lua = make_lua();
    const sol::object rulesObject = eval_lua(lua, R"(
        return {
            rules = {
                {
                    source = "broken",
                    regex = "x",
                    actions = {
                        { type = "log", message = "x" },
                    },
                },
            },
        }
    )");

    REQUIRE_THROWS_WITH(
        parse_exec_rules(rulesObject),
        Catch::Contains("source must be 'line' or 'screen'")
    );
}

TEST_CASE("exec rules parser rejects missing action fields", "[unit][exec_rules][parse]") {
    sol::state lua = make_lua();
    const sol::object rulesObject = eval_lua(lua, R"(
        return {
            rules = {
                {
                    source = "line",
                    regex = "x",
                    actions = {
                        { type = "progress" },
                    },
                },
            },
        }
    )");

    REQUIRE_THROWS_WITH(
        parse_exec_rules(rulesObject),
        Catch::Contains("progress action requires field 'percent', 'current', 'total', or 'speed'")
    );
}

TEST_CASE("exec rules parser accepts rich progress fields without percent", "[unit][exec_rules][parse]") {
    sol::state lua = make_lua();
    const sol::object rulesObject = eval_lua(lua, R"(
        return {
            rules = {
                {
                    source = "line",
                    regex = "^Loaded (.+)$",
                    actions = {
                        { type = "progress", current = "16.4", currentUnit = "MiB", total = "40.0", totalUnit = "MiB", speed = "2.5", speedUnit = "MiB/s" },
                    },
                },
            },
        }
    )");

    const ExecRuleset ruleset = parse_exec_rules(rulesObject);
    REQUIRE(ruleset.rules.size() == 1);
    CHECK(ruleset.rules[0].actions[0].fields.at("currentUnit") == "MiB");
    CHECK(ruleset.rules[0].actions[0].fields.at("speedUnit") == "MiB/s");
}

TEST_CASE("exec rules parser rejects unknown action type", "[unit][exec_rules][parse]") {
    sol::state lua = make_lua();
    const sol::object rulesObject = eval_lua(lua, R"(
        return {
            rules = {
                {
                    source = "line",
                    regex = "x",
                    actions = {
                        { type = "explode", message = "x" },
                    },
                },
            },
        }
    )");

    REQUIRE_THROWS_WITH(
        parse_exec_rules(rulesObject),
        Catch::Contains("action type 'explode' is unknown")
    );
}

TEST_CASE("exec rules parser rejects malformed top-level rules shape", "[unit][exec_rules][parse]") {
    sol::state lua = make_lua();

    REQUIRE_THROWS_WITH(
        parse_exec_rules(eval_lua(lua, "return 'not-a-table'")),
        Catch::Contains("rules must be a table")
    );

    REQUIRE_THROWS_WITH(
        parse_exec_rules(eval_lua(lua, "return { rules = 'broken' }")),
        Catch::Contains("rules.rules must be an array-style table")
    );
}

TEST_CASE("exec rules parser rejects invalid regex patterns", "[unit][exec_rules][parse]" ) {
    sol::state lua = make_lua();
    const sol::object rulesObject = eval_lua(lua, R"(
        return {
            rules = {
                {
                    source = "line",
                    regex = "(",
                    actions = {
                        { type = "log", message = "x" },
                    },
                },
            },
        }
    )");

    REQUIRE_THROWS_WITH(
        parse_exec_rules(rulesObject),
        Catch::Contains("regex failed to compile")
    );
}

TEST_CASE("exec rule runner mode is derived from rules", "[unit][exec_rules][parse]") {
    sol::state lua = make_lua();

    const ExecRuleset lineRules = parse_exec_rules(eval_lua(lua, R"(
        return {
            rules = {
                {
                    source = "line",
                    regex = "x",
                    actions = { { type = "log", message = "x" } },
                },
            },
        }
    )"));
    CHECK(determine_exec_rule_runner_mode(lineRules) == ExecRuleRunnerMode::Line);

    const ExecRuleset ptyRules = parse_exec_rules(eval_lua(lua, R"(
        return {
            rules = {
                {
                    source = "line",
                    regex = "x",
                    actions = { { type = "send", value = "y\n" } },
                },
            },
        }
    )"));
    CHECK(determine_exec_rule_runner_mode(ptyRules) == ExecRuleRunnerMode::Pty);
}
