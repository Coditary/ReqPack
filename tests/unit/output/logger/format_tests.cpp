#include <catch2/catch.hpp>

#include <cstdlib>

#include "output/logger.h"
#include "output/logger_core.h"
#include "output/command_output.h"

#include <mutex>
#include <utility>

namespace {

struct DisplayMessageRecord {
    std::string text;
    std::string source;
};

class RecordingDisplay final : public IDisplay {
public:
    void onSessionBegin(DisplayMode, const std::vector<std::string>&) override {}
    void onSessionEnd(bool, int, int, int) override {}

    void onItemBegin(const std::string&, const std::string&) override {}

    void onItemProgress(const std::string& itemId, const DisplayProgressMetrics& metrics) override {
        std::lock_guard<std::mutex> lock(mutex_);
        progresses.emplace_back(itemId, metrics);
    }

    void onItemStep(const std::string& itemId, const std::string& step) override {
        std::lock_guard<std::mutex> lock(mutex_);
        steps.emplace_back(itemId, step);
    }

    void onItemSuccess(const std::string& itemId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        successes.push_back(itemId);
    }

    void onItemFailure(const std::string& itemId, const std::string& reason) override {
        std::lock_guard<std::mutex> lock(mutex_);
        failures.emplace_back(itemId, reason);
    }

    void onMessage(const std::string& text, const std::string& source = {}) override {
        std::lock_guard<std::mutex> lock(mutex_);
        messages.push_back(DisplayMessageRecord{text, source});
    }

    void onTableBegin(const std::vector<std::string>&) override {}
    void onTableRow(const std::vector<std::string>&) override {}
    void onTableEnd() override {}
    void flush() override {}

    std::vector<std::pair<std::string, DisplayProgressMetrics>> progresses;
    std::vector<std::pair<std::string, std::string>> steps;
    std::vector<std::string> successes;
    std::vector<std::pair<std::string, std::string>> failures;
    std::vector<DisplayMessageRecord> messages;

private:
    std::mutex mutex_;
};

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const char* value) : name_(name) {
        if (const char* existing = std::getenv(name_)) {
            hadPrevious_ = true;
            previous_ = existing;
        }
        ::setenv(name_, value, 1);
    }

    ~ScopedEnvVar() {
        if (hadPrevious_) {
            ::setenv(name_, previous_.c_str(), 1);
        } else {
            ::unsetenv(name_);
        }
    }

private:
    const char* name_;
    bool hadPrevious_{false};
    std::string previous_;
};

} // namespace

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
        .action = OutputAction::PLUGIN_PROGRESS,
        .context = OutputContext{
            .source = "smoke",
            .scope = "search",
            .currentBytes = 17196646,
            .totalBytes = 41943040,
            .bytesPerSecond = 2621440,
        },
    }) == "[smoke] (search) progress=41%  16.4 MiB / 40.0 MiB  2.5 MiB/s");

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

TEST_CASE("progress metric helpers normalize and humanize values", "[unit][logger][format]") {
    CHECK(normalize_progress_units(1.5, "MiB").value() == 1572864);
    CHECK(normalize_progress_units(2.5, "MiB/s").value() == 2621440);
    CHECK(humanize_progress_bytes(17196646) == "16.4 MiB");
    CHECK(humanize_progress_rate(2621440) == "2.5 MiB/s");
    CHECK(resolve_progress_percent(DisplayProgressMetrics{.currentBytes = 17196646, .totalBytes = 41943040}).value() == 41);
}

TEST_CASE("logger render returns empty for flush and stop events", "[unit][logger][format]") {
    CHECK(logger_render_output_event(OutputEvent{.action = OutputAction::FLUSH}).empty());
    CHECK(logger_render_output_event(OutputEvent{.action = OutputAction::STOP}).empty());
}

TEST_CASE("logger routes item-scoped plugin callbacks to display lifecycle", "[unit][logger][display]") {
    Logger& logger = Logger::instance();
    RecordingDisplay display;

    logger.flushSync();
    logger.setConsoleOutput(false);
    logger.setDisplay(&display);

    logger.emit(OutputAction::PLUGIN_PROGRESS, OutputContext{.source = "smoke:demo", .scope = "smoke", .progressPercent = 42});
    logger.emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = "smoke:demo", .scope = "smoke", .eventName = "begin_step", .payload = "phase one"});
    logger.emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = "smoke:demo", .scope = "smoke", .eventName = "success", .payload = "ok"});
    logger.emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = "smoke:demo", .scope = "smoke", .eventName = "failed", .payload = "broken"});
    logger.flushSync();

    REQUIRE(display.progresses.size() == 1);
    CHECK(display.progresses[0].first == "smoke:demo");
    REQUIRE(display.progresses[0].second.percent.has_value());
    CHECK(display.progresses[0].second.percent.value() == 42);
    REQUIRE(display.steps.size() == 1);
    CHECK(display.steps[0].first == "smoke:demo");
    CHECK(display.steps[0].second == "phase one");
    REQUIRE(display.successes.size() == 1);
    CHECK(display.successes[0] == "smoke:demo");
    REQUIRE(display.failures.size() == 1);
    CHECK(display.failures[0].first == "smoke:demo");
    CHECK(display.failures[0].second == "broken");

    logger.setDisplay(nullptr);
    logger.setConsoleOutput(true);
    logger.flushSync();
}

TEST_CASE("logger routes non-item plugin callbacks as display messages", "[unit][logger][display]") {
    Logger& logger = Logger::instance();
    RecordingDisplay display;

    logger.flushSync();
    logger.setConsoleOutput(false);
    logger.setDisplay(&display);

    logger.emit(OutputAction::PLUGIN_PROGRESS, OutputContext{.source = "plugin", .scope = "smoke", .progressPercent = 7});
    logger.emit(OutputAction::PLUGIN_PROGRESS, OutputContext{.source = "plugin", .scope = "smoke", .currentBytes = 17196646, .totalBytes = 41943040, .bytesPerSecond = 2621440});
    logger.emit(OutputAction::PLUGIN_STATUS, OutputContext{.source = "plugin", .scope = "smoke", .statusCode = 17});
    logger.emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = "plugin", .scope = "smoke", .eventName = "begin_step", .payload = "ack y"});
    logger.emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = "plugin", .scope = "smoke", .eventName = "success", .payload = "ok"});
    logger.emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = "plugin", .scope = "smoke", .eventName = "failed", .payload = "soft-fail"});
    logger.emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = "plugin", .scope = "smoke", .eventName = "ack", .payload = "y"});
    logger.flushSync();

    REQUIRE(display.messages.size() == 7);
    CHECK(display.messages[0].source == "smoke");
    CHECK(display.messages[0].text == "progress 7%");
    CHECK(display.messages[1].text == "progress 41%  16.4 MiB / 40.0 MiB  2.5 MiB/s");
    CHECK(display.messages[2].text == "status 17");
    CHECK(display.messages[3].text == "ack y");
    CHECK(display.messages[4].text == "done");
    CHECK(display.messages[5].text == "failed: soft-fail");
    CHECK(display.messages[6].text == "ack: y");

    logger.setDisplay(nullptr);
    logger.setConsoleOutput(true);
    logger.flushSync();
}

TEST_CASE("logger hides technical package event messages from default display", "[unit][logger][display]") {
    Logger& logger = Logger::instance();
    RecordingDisplay display;

    logger.flushSync();
    logger.setConsoleOutput(false);
    logger.setDisplay(&display);

    logger.emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = "dnf:texlive-xurl", .scope = "dnf", .eventName = "installed", .payload = "{1=texlive-xurl}"});
    logger.emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = "dnf:texlive-xurl", .scope = "dnf", .eventName = "deleted", .payload = "{1=texlive-xurl}"});
    logger.emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = "dnf:texlive-xurl", .scope = "dnf", .eventName = "unavailable", .payload = "texlive-xurl"});
    logger.flushSync();

    REQUIRE(display.messages.size() == 1);
    CHECK(display.messages[0].source == "dnf:texlive-xurl");
    CHECK(display.messages[0].text == "unavailable: texlive-xurl");

    logger.setDisplay(nullptr);
    logger.setConsoleOutput(true);
    logger.flushSync();
}

TEST_CASE("command output renders info fields as wrapped key value lines", "[unit][logger][format]") {
    const ScopedEnvVar columns("COLUMNS", "50");

    CommandOutput output;
    output.mode = DisplayMode::INFO;
    output.sessionItems = {"dnf"};
    output.blocks.push_back(make_command_field_value_block({
        CommandOutputField{.key = "Name", .value = "texlive-xurl"},
        CommandOutputField{.key = "Description", .value = "Package xurl loads package url by default and defines possible url breaks for all alphanumerical characters and = / . : * - ~ ' \" All arguments which are valid for url can be used."},
    }));
    output.success = true;
    output.succeeded = 1;

    const std::string rendered = render_command_output_text(output);
    CHECK(rendered.find("Name        : texlive-xurl") != std::string::npos);
    CHECK(rendered.find("Description : Package xurl loads package url") != std::string::npos);
    CHECK(rendered.find("              default and defines possible url") != std::string::npos);
    CHECK(rendered.find("Field  Value") == std::string::npos);
}

TEST_CASE("command output keeps table layout for non-info field value blocks", "[unit][logger][format]") {
    const ScopedEnvVar columns("COLUMNS", "50");

    CommandOutput output;
    output.mode = DisplayMode::SNAPSHOT;
    output.sessionItems = {"snapshot"};
    output.blocks.push_back(make_command_field_value_block({
        CommandOutputField{.key = "Format", .value = "reqpack.lua"},
        CommandOutputField{.key = "Package Count", .value = "1"},
    }));
    output.success = true;
    output.succeeded = 1;

    const std::string rendered = render_command_output_text(output);
    CHECK(rendered.find("Field") != std::string::npos);
    CHECK(rendered.find("Value") != std::string::npos);
    CHECK(rendered.find("Package Count") != std::string::npos);
    CHECK(rendered.find(": reqpack.lua") == std::string::npos);
}
