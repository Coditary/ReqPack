#include <catch2/catch.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <chrono>

#include "output/ansi_color.h"
#include "output/color_display.h"
#include "output/diagnostic.h"
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

class TestColorDisplay final : public ColorDisplay {
public:
    using ColorDisplay::ColorDisplay;
    using ColorDisplay::decorateBarEmpty;
    using ColorDisplay::decorateBarFill;
    using ColorDisplay::decorateBarOuter;
    using ColorDisplay::decorateFailureMarker;
    using ColorDisplay::decorateHeader;
    using ColorDisplay::decorateMessage;
    using ColorDisplay::decorateRule;
    using ColorDisplay::decorateStep;
    using ColorDisplay::decorateSuccessMarker;
    using ColorDisplay::decorateSummaryFail;
    using ColorDisplay::decorateSummaryOk;
};

std::string padRight(const std::string& text, size_t width) {
    if (text.size() >= width) {
        return text;
    }
    return text + std::string(width - text.size(), ' ');
}

std::filesystem::path unique_temp_file(const std::string& prefix) {
    return std::filesystem::temp_directory_path() /
        (prefix + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
}

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

TEST_CASE("logger preserves trailing empty table cells", "[unit][logger][display]") {
    Logger& logger = Logger::instance();

    class RecordingTableDisplay final : public IDisplay {
    public:
        void onSessionBegin(DisplayMode, const std::vector<std::string>&) override {}
        void onSessionEnd(bool, int, int, int) override {}
        void onItemBegin(const std::string&, const std::string&) override {}
        void onItemProgress(const std::string&, const DisplayProgressMetrics&) override {}
        void onItemStep(const std::string&, const std::string&) override {}
        void onItemSuccess(const std::string&) override {}
        void onItemFailure(const std::string&, const std::string&) override {}
        void onMessage(const std::string&, const std::string&) override {}
        void onTableBegin(const std::vector<std::string>& headers) override {
            seenHeaders = headers;
        }
        void onTableRow(const std::vector<std::string>& cells) override {
            seenRows.push_back(cells);
        }
        void onTableEnd() override {}
        void flush() override {}

        std::vector<std::string> seenHeaders;
        std::vector<std::vector<std::string>> seenRows;
    } display;

    logger.flushSync();
    logger.setConsoleOutput(false);
    logger.setDisplay(&display);

    logger.displayTableHeader({"Name", "Version", "Type", "Architecture", "Description"});
    logger.displayTableRow({"org.example:demo-lib", "1.2.3", "jar", "", "Demo library artifact"});
    logger.displayTableEnd();
    logger.flushSync();

    REQUIRE(display.seenHeaders.size() == 5);
    REQUIRE(display.seenRows.size() == 1);
    REQUIRE(display.seenRows[0].size() == 5);
    CHECK(display.seenRows[0][0] == "org.example:demo-lib");
    CHECK(display.seenRows[0][1] == "1.2.3");
    CHECK(display.seenRows[0][2] == "jar");
    CHECK(display.seenRows[0][3].empty());
    CHECK(display.seenRows[0][4] == "Demo library artifact");

    logger.setDisplay(nullptr);
    logger.setConsoleOutput(true);
    logger.flushSync();
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

TEST_CASE("logger renders diagnostics for display and structured json", "[unit][logger][diagnostic]") {
    const DiagnosticMessage diagnostic = make_error_diagnostic(
        "network",
        "Download failed for https://example.invalid/pkg.tar.gz",
        "Remote host could not be reached.",
        "Check network access and URL.",
        "timeout",
        "downloader",
        "install",
        {{"url", "https://example.invalid/pkg.tar.gz"}}
    );

    CHECK(logger_render_display_message(diagnostic) ==
          "Download failed for https://example.invalid/pkg.tar.gz\nCause: Remote host could not be reached.\nFix: Check network access and URL.\ntimeout");

    const std::string json = logger_render_structured_event_json(OutputEvent{
        .action = OutputAction::DIAGNOSTIC,
        .context = OutputContext{
            .level = diagnostic.severity,
            .message = diagnostic.summary,
            .category = diagnostic.category,
            .cause = diagnostic.cause,
            .recommendation = diagnostic.recommendation,
            .details = diagnostic.details,
            .source = diagnostic.source,
            .scope = diagnostic.scope,
            .contextFields = diagnostic.context,
        },
    });

    CHECK(json.find("\"category\":\"network\"") != std::string::npos);
    CHECK(json.find("\"summary\":\"Download failed for https://example.invalid/pkg.tar.gz\"") != std::string::npos);
    CHECK(json.find("\"url\":\"https://example.invalid/pkg.tar.gz\"") != std::string::npos);
}

TEST_CASE("logger writes structured diagnostics and honors category filters", "[unit][logger][diagnostic]") {
    Logger& logger = Logger::instance();
    RecordingDisplay display;
    const std::filesystem::path logPath = unique_temp_file("reqpack-logger");

    logger.flushSync();
    logger.setDisplay(&display);
    logger.setConsoleOutput(false);
    logger.setEnabledCategories({"network"});
    logger.setCaptureDisplayEvents(true);
    logger.setStructuredFileSink(logPath.string());

    logger.diagnostic(make_error_diagnostic(
        "network",
        "Download failed",
        "Host unreachable.",
        "Check connection.",
        {},
        "downloader",
        "install"
    ));
    logger.diagnostic(make_error_diagnostic(
        "plugin",
        "Plugin action failed",
        "Plugin returned non-zero status.",
        "Inspect plugin output.",
        {},
        "npm",
        "install"
    ));
    logger.flushSync();

    std::ifstream input(logPath);
    REQUIRE(input.is_open());
    std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());

    CHECK(contents.find("\"category\":\"network\"") != std::string::npos);
    CHECK(contents.find("\"category\":\"plugin\"") == std::string::npos);
    REQUIRE_FALSE(display.messages.empty());
    CHECK(display.messages.front().text == "Download failed");

    logger.disableStructuredFileSink();
    logger.setEnabledCategories({});
    logger.setDisplay(nullptr);
    logger.setConsoleOutput(true);
    logger.flushSync();
    std::error_code error;
    std::filesystem::remove(logPath, error);
}

TEST_CASE("logger renders raw stdout log and display trace events", "[unit][logger][format]") {
    CHECK(logger_render_output_event(OutputEvent{
        .action = OutputAction::STDOUT,
        .context = OutputContext{.message = "plain", .source = "cli", .scope = "trace"},
    }) == "[cli] (trace) plain");

    CHECK(logger_render_output_event(OutputEvent{
        .action = OutputAction::LOG,
        .context = OutputContext{.message = "line", .source = "plugin"},
    }) == "[plugin] line");

    CHECK(logger_render_output_event(OutputEvent{
        .action = OutputAction::DISPLAY_SESSION_BEGIN,
        .context = OutputContext{.payload = "LIST dnf"},
    }) == "[display:session_begin] LIST dnf");

    CHECK(logger_render_output_event(OutputEvent{
        .action = OutputAction::DISPLAY_SESSION_END,
        .context = OutputContext{.payload = "done"},
    }) == "[display:session_end] done");

    CHECK(logger_render_output_event(OutputEvent{
        .action = OutputAction::DISPLAY_ITEM_BEGIN,
        .context = OutputContext{.message = "ripgrep", .source = "dnf:ripgrep"},
    }) == "[display:item_begin] dnf:ripgrep ripgrep");

    CHECK(logger_render_output_event(OutputEvent{
        .action = OutputAction::DISPLAY_ITEM_STEP,
        .context = OutputContext{.message = "Downloading", .source = "dnf:ripgrep"},
    }) == "[display:item_step] dnf:ripgrep Downloading");

    CHECK(logger_render_output_event(OutputEvent{
        .action = OutputAction::DISPLAY_ITEM_SUCCESS,
        .context = OutputContext{.source = "dnf:ripgrep"},
    }) == "[display:item_success] dnf:ripgrep");

    CHECK(logger_render_output_event(OutputEvent{
        .action = OutputAction::DISPLAY_ITEM_FAILURE,
        .context = OutputContext{.message = "failed", .source = "dnf:ripgrep"},
    }) == "[display:item_failure] dnf:ripgrep failed");

    CHECK(logger_render_output_event(OutputEvent{
        .action = OutputAction::DISPLAY_MESSAGE,
        .context = OutputContext{.message = "hello"},
    }) == "[display:message] hello");

    CHECK(logger_render_output_event(OutputEvent{
        .action = OutputAction::DISPLAY_TABLE_HEADER,
        .context = OutputContext{.payload = "Name|Version"},
    }) == "[display:table_header] Name|Version");

    CHECK(logger_render_output_event(OutputEvent{
        .action = OutputAction::DISPLAY_TABLE_ROW,
        .context = OutputContext{.payload = "ripgrep|14.1"},
    }) == "[display:table_row] ripgrep|14.1");

    CHECK(logger_render_output_event(OutputEvent{
        .action = OutputAction::DISPLAY_TABLE_END,
        .context = {},
    }) == "[display:table_end]");
}

TEST_CASE("color display wraps decorated segments with configured ansi styles", "[unit][display][color]") {
    TestColorDisplay display(DisplayColorScheme{
        .rule = "underline blue",
        .header = "bold",
        .summaryOk = "green",
        .summaryFail = "red",
        .barFill = "yellow",
        .barEmpty = "dim",
        .step = "cyan",
        .successMarker = "bright_green",
        .failureMarker = "bright_red",
        .message = "magenta",
    });

    CHECK(display.decorateRule("-----") == ansi_wrap("-----", "underline blue"));
    CHECK(display.decorateHeader("LIST: dnf") == ansi_wrap("LIST: dnf", "bold"));
    CHECK(display.decorateSummaryOk("done") == ansi_wrap("done", "green"));
    CHECK(display.decorateSummaryFail("failed") == ansi_wrap("failed", "red"));
    CHECK(display.decorateBarFill(3) == ansi_wrap("###", "yellow"));
    CHECK(display.decorateBarEmpty(2) == ansi_wrap("..", "dim"));
    CHECK(display.decorateStep("Downloading") == ansi_wrap("Downloading", "cyan"));
    CHECK(display.decorateSuccessMarker("OK") == ansi_wrap("OK", "bright_green"));
    CHECK(display.decorateFailureMarker("[FAILED]") == ansi_wrap("[FAILED]", "bright_red"));
    CHECK(display.decorateMessage("hello") == ansi_wrap("hello", "magenta"));
}

TEST_CASE("color display colors only progress bar brackets when configured", "[unit][display][color]") {
    const std::string inner = ansi_wrap("###..", "green");

    SECTION("empty and none bar specs keep plain brackets") {
        CHECK(TestColorDisplay(DisplayColorScheme{}).decorateBarOuter(inner) == "[" + inner + "]");
        CHECK(TestColorDisplay(DisplayColorScheme{.barOuter = "none"}).decorateBarOuter(inner) == "[" + inner + "]");
    }

    SECTION("bar outer spec wraps brackets only") {
        const std::string spec = "bold blue";
        const std::string expected = ansi_open(spec) + "[" + ansi_reset(spec) + inner + ansi_open(spec) + "]" + ansi_reset(spec);
        CHECK(TestColorDisplay(DisplayColorScheme{.barOuter = spec}).decorateBarOuter(inner) == expected);
    }
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

TEST_CASE("command output wraps oversized package tables to terminal width", "[unit][logger][format]") {
    const ScopedEnvVar columns("COLUMNS", "72");

    CommandOutput output;
    output.mode = DisplayMode::SEARCH;
    output.sessionItems = {"dnf"};
    output.blocks.push_back(make_command_table_block(
        {"System", "Name", "Version", "Type", "Architecture", "Description"},
        {{"dnf", "python3-snakemake-storage-plugin-webdav.noarch", "1.2.3", "plugin", "noarch", "A Snakemake storage plugin for downloading input files from HTTP(s)"}}
    ));
    output.success = true;
    output.succeeded = 1;

    const std::string rendered = render_command_output_text(output);
    CHECK(rendered.find("python3-") != std::string::npos);
    CHECK(rendered.find("webdav") != std::string::npos);
    CHECK(rendered.find("plugin") != std::string::npos);
    CHECK(rendered.find("noarch") != std::string::npos);
    CHECK(rendered.find("storage") != std::string::npos);
    CHECK(rendered.find("HTTP(s)") != std::string::npos);
    CHECK(rendered.find("webdav") != std::string::npos);
}

TEST_CASE("command output wraps normalized list and outdated tables to terminal width", "[unit][logger][format]") {
    const ScopedEnvVar columns("COLUMNS", "76");

    SECTION("list table uses search-style wrapped layout") {
        CommandOutput output;
        output.mode = DisplayMode::LIST;
        output.sessionItems = {"dnf"};
        output.blocks.push_back(make_command_table_block(
            {"Name", "Version", "Type", "Architecture", "Description"},
            {{"python3-snakemake-storage-plugin-webdav", "1.2.3", "plugin", "noarch", "A Snakemake storage plugin for downloading input files from HTTP(s)"}}
        ));
        output.success = true;
        output.succeeded = 1;

        const std::string rendered = render_command_output_text(output);
        CHECK(rendered.find("LIST: dnf") != std::string::npos);
        CHECK(rendered.find("plugin") != std::string::npos);
        CHECK(rendered.find("noarch") != std::string::npos);
        CHECK(rendered.find("Snakemake storage") != std::string::npos);
    }

    SECTION("outdated table wraps with installed and latest columns") {
        CommandOutput output;
        output.mode = DisplayMode::OUTDATED;
        output.sessionItems = {"dnf"};
        output.blocks.push_back(make_command_table_block(
            {"Name", "Installed", "Latest", "Type", "Architecture", "Description"},
            {{"python3-snakemake-storage-plugin-webdav", "1.2.3-1.fc43", "1.2.4-1.fc43", "plugin", "noarch", "A Snakemake storage plugin for downloading input files from HTTP(s)"}}
        ));
        output.success = true;
        output.succeeded = 1;

        const std::string rendered = render_command_output_text(output);
        CHECK(rendered.find("OUTDATED: dnf") != std::string::npos);
        CHECK(rendered.find("Installed") != std::string::npos);
        CHECK(rendered.find("Latest") != std::string::npos);
        CHECK(rendered.find("1.2.4-1.fc43") != std::string::npos);
        CHECK(rendered.find("plugin") != std::string::npos);
        CHECK(rendered.find("noarch") != std::string::npos);
        CHECK(rendered.find("...") != std::string::npos);
    }
}

TEST_CASE("command output uses preferred package table widths on wide terminals", "[unit][logger][format]") {
    const ScopedEnvVar columns("COLUMNS", "160");

    CommandOutput output;
    output.mode = DisplayMode::LIST;
    output.sessionItems = {"maven"};
    output.blocks.push_back(make_command_table_block(
        {"Name", "Version", "Type", "Architecture", "Description"},
        {{"org.apache.maven.plugins:maven-clean-plugin", "3.2.0", "maven-plugin", "", "Apache Maven Clean Plugin"}}
    ));
    output.success = true;
    output.succeeded = 1;

    const std::string rendered = render_command_output_text(output);
    const std::string expectedHeader = padRight("Name", 50)
        + "  " + padRight("Version", 16)
        + "  " + padRight("Type", 14)
        + "  " + padRight("Architecture", 12)
        + "  Description";
    const std::string expectedRow = padRight("org.apache.maven.plugins:maven-clean-plugin", 50)
        + "  " + padRight("3.2.0", 16)
        + "  " + padRight("maven-plugin", 14)
        + "  " + padRight("", 12)
        + "  Apache Maven Clean Plugin";

    CHECK(rendered.find(expectedHeader) != std::string::npos);
    CHECK(rendered.find(expectedRow) != std::string::npos);
}

TEST_CASE("command output uses preferred outdated table widths on wide terminals", "[unit][logger][format]") {
    const ScopedEnvVar columns("COLUMNS", "160");

    CommandOutput output;
    output.mode = DisplayMode::OUTDATED;
    output.sessionItems = {"dnf"};
    output.blocks.push_back(make_command_table_block(
        {"Name", "Installed", "Latest", "Type", "Architecture", "Description"},
        {{"python3-snakemake-storage-plugin-webdav", "1.2.3-1.fc43", "1.2.4-1.fc43", "plugin", "noarch", "A Snakemake storage plugin for downloading input files from HTTP(s)"}}
    ));
    output.success = true;
    output.succeeded = 1;

    const std::string rendered = render_command_output_text(output);
    const std::string expectedHeader = padRight("Name", 50)
        + "  " + padRight("Installed", 16)
        + "  " + padRight("Latest", 16)
        + "  " + padRight("Type", 14)
        + "  " + padRight("Architecture", 12)
        + "  Description";

    CHECK(rendered.find(expectedHeader) != std::string::npos);
    CHECK(rendered.find("python3-snakemake-storage-plugin-webdav") != std::string::npos);
    CHECK(rendered.find("1.2.3-1.fc43") != std::string::npos);
    CHECK(rendered.find("1.2.4-1.fc43") != std::string::npos);
}
