#include <catch2/catch.hpp>

#include <boost/graph/adjacency_list.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

#include "core/export/audit_exporter.h"
#include "output/logger.h"

namespace {

class TempDir {
public:
    explicit TempDir(const std::string& prefix)
        : path_(std::filesystem::temp_directory_path() /
            (prefix + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class StaticMetadataProvider final : public PluginMetadataProvider {
public:
    std::map<std::string, PluginSecurityMetadata> metadata;

    std::optional<PluginSecurityMetadata> getPluginSecurityMetadata(const std::string& name) override {
        const auto it = metadata.find(name);
        if (it == metadata.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::vector<std::string> getKnownPluginNames() override {
        std::vector<std::string> names;
        for (const auto& [name, _] : metadata) {
            names.push_back(name);
        }
        return names;
    }
};

class ScopedColumnsEnv {
public:
    explicit ScopedColumnsEnv(const char* value) {
        if (const char* existing = std::getenv("COLUMNS")) {
            hadValue_ = true;
            previousValue_ = existing;
        }

        if (value != nullptr) {
            ::setenv("COLUMNS", value, 1);
        } else {
            ::unsetenv("COLUMNS");
        }
    }

    ~ScopedColumnsEnv() {
        if (hadValue_) {
            ::setenv("COLUMNS", previousValue_.c_str(), 1);
        } else {
            ::unsetenv("COLUMNS");
        }
    }

private:
    bool hadValue_ = false;
    std::string previousValue_;
};

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* key, const char* value) : key_(key) {
        if (const char* existing = std::getenv(key_)) {
            hadValue_ = true;
            previousValue_ = existing;
        }

        if (value != nullptr) {
            ::setenv(key_, value, 1);
        } else {
            ::unsetenv(key_);
        }
    }

    ~ScopedEnvVar() {
        if (hadValue_) {
            ::setenv(key_, previousValue_.c_str(), 1);
        } else {
            ::unsetenv(key_);
        }
    }

private:
    const char* key_;
    bool hadValue_ = false;
    std::string previousValue_;
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

Graph make_graph() {
    Graph graph;
    const auto react = boost::add_vertex(Package{.action = ActionType::AUDIT, .system = "npm", .name = "react", .version = "18.3.1"}, graph);
    const auto scheduler = boost::add_vertex(Package{.action = ActionType::AUDIT, .system = "npm", .name = "scheduler", .version = "0.24.0"}, graph);
    boost::add_edge(scheduler, react, graph);
    return graph;
}

std::vector<ValidationFinding> make_findings() {
    return {
        ValidationFinding{
            .id = "CVE-2026-1234",
            .kind = "vulnerability",
            .package = Package{.action = ActionType::AUDIT, .system = "npm", .name = "react", .version = "18.3.1"},
            .source = "osv",
            .severity = "high",
            .score = 8.8,
            .message = "react issue",
        }
    };
}

class RecordingDisplay final : public IDisplay {
public:
    void onSessionBegin(DisplayMode, const std::vector<std::string>&) override {}
    void onSessionEnd(bool, int, int, int) override {}
    void onItemBegin(const std::string&, const std::string&) override {}
    void onItemProgress(const std::string&, const DisplayProgressMetrics&) override {}
    void onItemStep(const std::string&, const std::string&) override {}
    void onItemSuccess(const std::string&) override {}
    void onItemFailure(const std::string&, const std::string&) override {}
    void onTableBegin(const std::vector<std::string>&) override {}
    void onTableRow(const std::vector<std::string>&) override {}
    void onTableEnd() override {}
    void flush() override {}

    void onMessage(const std::string& text, const std::string& source = {}) override {
        messages.push_back(source.empty() ? text : ("[" + source + "] " + text));
    }

    std::vector<std::string> messages;
};

class ScopedLoggerDisplay {
public:
    explicit ScopedLoggerDisplay(IDisplay* display)
        : logger_(Logger::instance()) {
        logger_.flushSync();
        logger_.setDisplay(display);
    }

    ~ScopedLoggerDisplay() {
        logger_.flushSync();
        logger_.setDisplay(nullptr);
        logger_.flushSync();
    }

private:
    Logger& logger_;
};

}  // namespace

TEST_CASE("audit exporter renders default table output", "[unit][audit][export]") {
    ScopedColumnsEnv columns{"72"};
    ScopedEnvVar noColor{"NO_COLOR", "1"};
    ScopedEnvVar forceColor{"FORCE_COLOR", nullptr};
    AuditExporter exporter;
    Request request;
    request.action = ActionType::AUDIT;

    Graph graph;
    boost::add_vertex(Package{.action = ActionType::AUDIT, .system = "maven", .name = "org.apache.logging.log4j:log4j-core", .version = "2.13.1"}, graph);
    const std::vector<ValidationFinding> findings{
        ValidationFinding{
            .id = "GHSA-3pxv-7cmr-fjr4",
            .kind = "vulnerability",
            .package = Package{.action = ActionType::AUDIT, .system = "maven", .name = "org.apache.logging.log4j:log4j-core", .version = "2.13.1"},
            .source = "osv",
            .severity = "medium",
            .score = 4.0,
            .message = "alpha beta gamma delta epsilon",
        }
    };

    const std::string rendered = exporter.renderGraph(graph, findings, request);
    CHECK(rendered.find('\t') == std::string::npos);
    CHECK(rendered.find("SYSTEM NAME         VERSION FINDING      SEVERITY SCORE MESSAGE") != std::string::npos);
    CHECK(rendered.find("maven  org.apach... 2.13.1  GHSA-3pxv... medium   4     alpha beta gamma") != std::string::npos);
    CHECK(rendered.find(std::string{"\n"} + std::string(56, ' ') + "delta epsilon\n") != std::string::npos);
}

TEST_CASE("audit exporter colorizes severity in terminal table output", "[unit][audit][export]") {
    ScopedColumnsEnv columns{"72"};
    ScopedEnvVar noColor{"NO_COLOR", nullptr};
    ScopedEnvVar forceColor{"FORCE_COLOR", "1"};
    AuditExporter exporter;
    Request request;
    request.action = ActionType::AUDIT;

    const std::string rendered = exporter.renderGraph(make_graph(), make_findings(), request);
    CHECK(rendered.find("\033[1;31mhigh    \033[0m") != std::string::npos);
}

TEST_CASE("audit exporter supports no-wrap and wide table flags", "[unit][audit][export]") {
    ScopedColumnsEnv columns{"72"};
    ScopedEnvVar noColor{"NO_COLOR", "1"};
    ScopedEnvVar forceColor{"FORCE_COLOR", nullptr};
    AuditExporter exporter;
    Request request;
    request.action = ActionType::AUDIT;
    request.flags = {"wide", "no-wrap"};

    Graph graph;
    boost::add_vertex(Package{.action = ActionType::AUDIT, .system = "maven", .name = "org.apache.logging.log4j:log4j-core", .version = "2.13.1"}, graph);
    const std::vector<ValidationFinding> findings{
        ValidationFinding{
            .id = "GHSA-3pxv-7cmr-fjr4",
            .kind = "vulnerability",
            .package = Package{.action = ActionType::AUDIT, .system = "maven", .name = "org.apache.logging.log4j:log4j-core", .version = "2.13.1"},
            .source = "osv",
            .severity = "medium",
            .score = 4.0,
            .message = "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu",
        }
    };

    const std::string rendered = exporter.renderGraph(graph, findings, request);
    CHECK(rendered.find("alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu") != std::string::npos);
    CHECK(rendered.find(std::string{"\n"} + std::string(56, ' ') + "delta epsilon") == std::string::npos);
}

TEST_CASE("audit exporter keeps file exports plain even when color forced", "[unit][audit][export]") {
    ScopedColumnsEnv columns{"72"};
    ScopedEnvVar noColor{"NO_COLOR", nullptr};
    ScopedEnvVar forceColor{"FORCE_COLOR", "1"};
    AuditExporter exporter;
    Request request;
    request.action = ActionType::AUDIT;
    request.outputPath = "audit.txt";
    request.outputFormat = "table";

    const std::string rendered = exporter.renderGraph(make_graph(), make_findings(), request);
    CHECK(rendered.find("\033[") == std::string::npos);
}

TEST_CASE("audit exporter renders clean table summary", "[unit][audit][export]") {
    AuditExporter exporter;
    Request request;
    request.action = ActionType::AUDIT;

    const std::string rendered = exporter.renderGraph(make_graph(), {}, request);
    CHECK(rendered.find("No vulnerabilities or audit findings detected.") != std::string::npos);
}

TEST_CASE("audit exporter renders reqpack json output", "[unit][audit][export]") {
    AuditExporter exporter;
    Request request;
    request.action = ActionType::AUDIT;
    request.outputFormat = "json";

    const std::string rendered = exporter.renderGraph(make_graph(), make_findings(), request);
    CHECK(rendered.find("\"packages\"") != std::string::npos);
    CHECK(rendered.find("\"findings\"") != std::string::npos);
    CHECK(rendered.find("\"id\": \"CVE-2026-1234\"") != std::string::npos);
}

TEST_CASE("audit exporter defaults json file output to cyclonedx vex json", "[unit][audit][export]") {
    TempDir tempDir{"reqpack-audit-export"};
    AuditExporter exporter;
    Request request;
    request.action = ActionType::AUDIT;
    request.outputPath = (tempDir.path() / "audit.json").string();

    REQUIRE(exporter.exportGraph(make_graph(), make_findings(), request));
    const std::string rendered = read_file(request.outputPath);
    CHECK(rendered.find("\"bomFormat\": \"CycloneDX\"") != std::string::npos);
    CHECK(rendered.find("\"vulnerabilities\"") != std::string::npos);
    CHECK(rendered.find("\"analysis\": {\"state\": \"in_triage\", \"detail\": \"Matched by ReqPack audit from local vulnerability data. Reachability and exploitability were not analyzed.\"}") != std::string::npos);
    CHECK(rendered.find("\"ratings\": [{\"source\": {\"name\": \"osv\"}, \"severity\": \"high\", \"score\": 8.8}]") != std::string::npos);
}

TEST_CASE("audit exporter keeps unresolved findings in triage state", "[unit][audit][export]") {
    AuditExporter exporter;
    Request request;
    request.action = ActionType::AUDIT;
    request.outputFormat = "cyclonedx-vex-json";

    Graph graph;
    boost::add_vertex(Package{.action = ActionType::AUDIT, .system = "npm", .name = "left-pad"}, graph);
    const std::vector<ValidationFinding> findings{
        ValidationFinding{
            .kind = "unresolved_version",
            .package = Package{.action = ActionType::AUDIT, .system = "npm", .name = "left-pad"},
            .source = "osv",
            .severity = "low",
            .message = "package version unavailable for vulnerability matching",
        }
    };

    const std::string rendered = exporter.renderGraph(graph, findings, request);
    CHECK(rendered.find("\"analysis\": {\"state\": \"in_triage\", \"detail\": \"ReqPack could not resolve package version. Vulnerability matching may be incomplete.\"}") != std::string::npos);
}

TEST_CASE("audit exporter infers sarif from file extension", "[unit][audit][export]") {
    TempDir tempDir{"reqpack-audit-sarif"};
    AuditExporter exporter;
    Request request;
    request.action = ActionType::AUDIT;
    request.outputPath = (tempDir.path() / "audit.sarif").string();

    REQUIRE(exporter.exportGraph(make_graph(), make_findings(), request));
    const std::string rendered = read_file(request.outputPath);
    CHECK(rendered.find("\"version\": \"2.1.0\"") != std::string::npos);
    CHECK(rendered.find("\"runs\"") != std::string::npos);
    CHECK(rendered.find("\"ruleId\": \"CVE-2026-1234\"") != std::string::npos);
}

TEST_CASE("audit exporter reports file-open failure through logger diagnostics", "[unit][audit][export]") {
    TempDir tempDir{"reqpack-audit-open-failure"};
    AuditExporter exporter;
    Request request;
    request.action = ActionType::AUDIT;
    request.outputPath = tempDir.path().string();
    request.flags = {"force"};

    RecordingDisplay display;
    ScopedLoggerDisplay displayGuard(&display);

    CHECK_FALSE(exporter.exportGraph(make_graph(), make_findings(), request));
    Logger::instance().flushSync();
    CHECK(std::any_of(display.messages.begin(), display.messages.end(), [&](const std::string& message) {
        return message.find("failed to open audit output path: " + tempDir.path().string()) != std::string::npos;
    }));
    CHECK(std::any_of(display.messages.begin(), display.messages.end(), [](const std::string& message) {
        return message.find("Cause: ReqPack could not open requested audit output file for writing.") != std::string::npos;
    }));
}

TEST_CASE("audit exporter formats maven purls in cyclonedx vex output", "[unit][audit][export]") {
    StaticMetadataProvider metadataProvider;
    metadataProvider.metadata["maven"] = PluginSecurityMetadata{
        .osvEcosystem = "Maven",
        .purlType = "maven",
        .versionComparator = VersionComparatorSpec{.profile = "maven-comparable"},
    };

    AuditExporter exporter(&metadataProvider);
    Request request;
    request.action = ActionType::AUDIT;
    request.outputFormat = "cyclonedx-vex-json";

    Graph graph;
    boost::add_vertex(Package{.action = ActionType::AUDIT, .system = "maven", .name = "org.slf4j:slf4j-api", .version = "2.0.16"}, graph);
    const std::vector<ValidationFinding> findings{
        ValidationFinding{
            .id = "GHSA-demo",
            .kind = "vulnerability",
            .package = Package{.action = ActionType::AUDIT, .system = "maven", .name = "org.slf4j:slf4j-api", .version = "2.0.16"},
            .source = "osv",
            .severity = "medium",
            .score = 5.0,
            .message = "demo",
        }
    };

    const std::string rendered = exporter.renderGraph(graph, findings, request);
    CHECK(rendered.find("\"purl\": \"pkg:maven/org.slf4j/slf4j-api@2.0.16\"") != std::string::npos);
}
