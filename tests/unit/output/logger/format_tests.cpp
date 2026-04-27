#include <catch2/catch.hpp>

#include "output/logger_core.h"

TEST_CASE("logger formats source and scope prefixes", "[unit][logger][format]") {
    CHECK(logger_format_message(OutputContext{.message = "hello"}) == "hello");
    CHECK(logger_format_message(OutputContext{.message = "hello", .source = "cli"}) == "[cli] hello");
    CHECK(logger_format_message(OutputContext{.message = "hello", .scope = "info"}) == "(info) hello");
    CHECK(logger_format_message(OutputContext{.message = "hello", .source = "cli", .scope = "info"}) == "[cli] (info) hello");
}

TEST_CASE("logger renders plugin output strings", "[unit][logger][format]") {
    CHECK(logger_render_output_event(OutputEvent{
        .action = OutputAction::PLUGIN_STATUS,
        .context = OutputContext{.source = "smoke", .scope = "search", .statusCode = 7},
    }) == "[smoke] (search) status=7");

    CHECK(logger_render_output_event(OutputEvent{
        .action = OutputAction::PLUGIN_PROGRESS,
        .context = OutputContext{.source = "smoke", .scope = "search", .progressPercent = 42},
    }) == "[smoke] (search) progress=42%");

    CHECK(logger_render_output_event(OutputEvent{
        .action = OutputAction::PLUGIN_EVENT,
        .context = OutputContext{.source = "smoke", .scope = "search", .eventName = "ack", .payload = "y"},
    }) == "[smoke] (search) ack: y");

    CHECK(logger_render_output_event(OutputEvent{
        .action = OutputAction::PLUGIN_ARTIFACT,
        .context = OutputContext{.source = "smoke", .scope = "search", .payload = "report.json"},
    }) == "[smoke] (search) artifact: report.json");
}

TEST_CASE("logger stdout newline decision handles empty and terminated messages", "[unit][logger][format]") {
    CHECK(logger_stdout_needs_trailing_newline(OutputContext{.message = ""}));
    CHECK(logger_stdout_needs_trailing_newline(OutputContext{.message = "hello"}));
    CHECK_FALSE(logger_stdout_needs_trailing_newline(OutputContext{.message = "hello\n"}));
}

TEST_CASE("logger render returns empty for flush and stop events", "[unit][logger][format]") {
    CHECK(logger_render_output_event(OutputEvent{.action = OutputAction::FLUSH}).empty());
    CHECK(logger_render_output_event(OutputEvent{.action = OutputAction::STOP}).empty());
}
