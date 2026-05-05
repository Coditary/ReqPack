#include <catch2/catch.hpp>

#include <boost/graph/adjacency_list.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <system_error>
#include <vector>

#include "core/validator.h"

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

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.is_open());
    output << content;
}

Graph make_graph() {
    Graph graph;
    boost::add_vertex(Package{.action = ActionType::INSTALL, .system = "dnf", .name = "ripgrep"}, graph);
    return graph;
}

Graph make_remove_graph() {
    Graph graph;
    boost::add_vertex(Package{.action = ActionType::REMOVE, .system = "dnf", .name = "ripgrep", .version = "14.1"}, graph);
    return graph;
}

Graph make_mixed_graph() {
    Graph graph;
    boost::add_vertex(Package{.action = ActionType::REMOVE, .system = "dnf", .name = "ripgrep", .version = "14.1"}, graph);
    boost::add_vertex(Package{.action = ActionType::INSTALL, .system = "dnf", .name = "jq", .version = "1.7"}, graph);
    return graph;
}

ValidationFinding make_finding(const std::string& severity, double score = 0.0) {
    ValidationFinding finding;
    finding.id = severity + std::to_string(score);
    finding.severity = severity;
    finding.score = score;
    return finding;
}

class TestValidator : public Validator {
public:
    using Validator::Validator;

    std::vector<ValidationFinding> findings;
    bool promptResponse{true};
    mutable int promptCalls{0};
    mutable int reportCalls{0};

protected:
    void expect_same_findings(const std::vector<ValidationFinding>& actual) const {
        REQUIRE(actual.size() == findings.size());
        for (std::size_t i = 0; i < actual.size(); ++i) {
            CHECK(actual[i].severity == findings[i].severity);
            CHECK(actual[i].score == findings[i].score);
            CHECK(actual[i].message == findings[i].message);
            CHECK(actual[i].source == findings[i].source);
            CHECK(actual[i].id == findings[i].id);
            CHECK(actual[i].kind == findings[i].kind);
        }
    }

    std::vector<ValidationFinding> scanGraph(const Graph& graph) const override {
        (void)graph;
        return findings;
    }

    bool requestUserDecision(const std::vector<ValidationFinding>& requestedFindings) const override {
        expect_same_findings(requestedFindings);
        ++promptCalls;
        return promptResponse;
    }

    void generateReport(const Graph& graph, const std::vector<ValidationFinding>& reportedFindings) const override {
        (void)graph;
        expect_same_findings(reportedFindings);
        ++reportCalls;
    }
};

class PromptingValidator : public Validator {
public:
    using Validator::Validator;

    std::vector<ValidationFinding> findings;

protected:
    std::vector<ValidationFinding> scanGraph(const Graph& graph) const override {
        (void)graph;
        return findings;
    }
};

class StreamBufferGuard {
public:
    StreamBufferGuard(std::ios& stream, std::streambuf* replacement)
        : stream_(stream), previous_(stream.rdbuf(replacement)) {}

    ~StreamBufferGuard() {
        stream_.rdbuf(previous_);
    }

private:
    std::ios& stream_;
    std::streambuf* previous_;
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

}  // namespace

TEST_CASE("validator returns nullptr for null graph", "[unit][validator][validate]") {
    Validator validator;
    CHECK(validator.validate(nullptr) == nullptr);
}

TEST_CASE("validator aborts immediately when findings exceed threshold and prompting is disabled", "[unit][validator][validate]") {
    ReqPackConfig config;
    config.security.severityThreshold = SeverityLevel::HIGH;

    TestValidator validator(nullptr, config);
    validator.findings = {make_finding("critical")};

    Graph graph = make_graph();
    CHECK(validator.validate(&graph) == nullptr);
    CHECK(validator.promptCalls == 0);
}

TEST_CASE("validator stores last findings for aborted validation", "[unit][validator][validate]") {
    ReqPackConfig config;
    config.security.severityThreshold = SeverityLevel::HIGH;

    TestValidator validator(nullptr, config);
    validator.findings = {make_finding("critical")};

    Graph graph = make_graph();
    CHECK(validator.validate(&graph) == nullptr);
    REQUIRE(validator.getLastFindings().size() == 1);
    CHECK(validator.getLastFindings().front().severity == "critical");

    CHECK(validator.validate(nullptr) == nullptr);
    CHECK(validator.getLastFindings().empty());
}

TEST_CASE("validator audit returns findings without applying abort disposition", "[unit][validator][audit]") {
    ReqPackConfig config;
    config.security.severityThreshold = SeverityLevel::CRITICAL;
    config.security.onUnsafe = UnsafeAction::ABORT;

    TestValidator validator(nullptr, config);
    validator.findings = {make_finding("critical")};

    Graph graph = make_graph();
    const std::vector<ValidationFinding> findings = validator.audit(&graph);
    REQUIRE(findings.size() == 1);
    CHECK(findings.front().severity == "critical");
    REQUIRE(validator.getLastFindings().size() == 1);
    CHECK(validator.getLastFindings().front().severity == "critical");
}

TEST_CASE("validator prompts once when unsafe findings require user approval", "[unit][validator][validate]") {
    ReqPackConfig config;
    config.security.severityThreshold = SeverityLevel::HIGH;
    config.security.promptOnUnsafe = true;

    TestValidator validator(nullptr, config);
    validator.findings = {make_finding("critical")};

    Graph graph = make_graph();
    CHECK(validator.validate(&graph) == &graph);
    CHECK(validator.promptCalls == 1);
}

TEST_CASE("validator aborts when user declines prompted run", "[unit][validator][validate]") {
    ReqPackConfig config;
    config.security.promptOnUnsafe = true;
    config.security.severityThreshold = SeverityLevel::CRITICAL;

    TestValidator validator(nullptr, config);
    validator.findings = {make_finding("low")};
    validator.promptResponse = false;

    Graph graph = make_graph();
    CHECK(validator.validate(&graph) == nullptr);
    CHECK(validator.promptCalls == 1);
}

TEST_CASE("validator accepts interactive confirmation from stdin", "[unit][validator][validate]") {
    ReqPackConfig config;
    config.security.promptOnUnsafe = true;
    config.security.severityThreshold = SeverityLevel::CRITICAL;

    PromptingValidator validator(nullptr, config);
    validator.findings = {make_finding("low")};

    Graph graph = make_graph();
    std::istringstream input("y\n");
    std::ostringstream errorOutput;
    StreamBufferGuard inputGuard(std::cin, input.rdbuf());
    StreamBufferGuard errorGuard(std::cerr, errorOutput.rdbuf());

    CHECK(validator.validate(&graph) == &graph);
    CHECK(errorOutput.str().find("unsafe findings require confirmation") != std::string::npos);
    CHECK(errorOutput.str().find("Continue? [y/N]") != std::string::npos);
}

TEST_CASE("validator denies prompted run when interactive mode is disabled", "[unit][validator][validate]") {
    ReqPackConfig config;
    config.security.promptOnUnsafe = true;
    config.security.severityThreshold = SeverityLevel::CRITICAL;
    config.interaction.interactive = false;

    PromptingValidator validator(nullptr, config);
    validator.findings = {make_finding("low")};

    Graph graph = make_graph();
    std::ostringstream errorOutput;
    StreamBufferGuard errorGuard(std::cerr, errorOutput.rdbuf());

    CHECK(validator.validate(&graph) == nullptr);
    CHECK(errorOutput.str().find("interactive mode is disabled") != std::string::npos);
}

TEST_CASE("validator returns input graph and generates report for safe findings", "[unit][validator][validate]") {
    ReqPackConfig config;
    config.reports.enabled = true;

    TestValidator validator(nullptr, config);
    validator.findings = {};

    Graph graph = make_graph();
    CHECK(validator.validate(&graph) == &graph);
    CHECK(validator.promptCalls == 0);
    CHECK(validator.reportCalls == 1);
}

TEST_CASE("validator applies allow rules before threshold evaluation", "[unit][validator][validate]") {
    ReqPackConfig config;
    config.security.severityThreshold = SeverityLevel::HIGH;
    config.security.allowVulnerabilityIds = {"critical0.000000"};

    TestValidator validator(nullptr, config);
    validator.findings = {make_finding("critical")};

    Graph graph = make_graph();
    CHECK(validator.validate(&graph) == &graph);
    CHECK(validator.promptCalls == 0);
}

TEST_CASE("validator does not block remove-only graph for vulnerability findings", "[unit][validator][validate]") {
    ReqPackConfig config;
    config.security.severityThreshold = SeverityLevel::CRITICAL;
    config.security.onUnsafe = UnsafeAction::ABORT;

    TestValidator validator(nullptr, config);
    ValidationFinding finding = make_finding("critical", 10.0);
    finding.kind = "vulnerability";
    finding.id = "GHSA-demo";
    validator.findings = {finding};

    Graph graph = make_remove_graph();
    CHECK(validator.validate(&graph) == &graph);
    REQUIRE(validator.getLastFindings().size() == 1);
    CHECK(validator.getLastFindings().front().id == "GHSA-demo");
}

TEST_CASE("validator still blocks mixed-action graph for vulnerability findings", "[unit][validator][validate]") {
    ReqPackConfig config;
    config.security.severityThreshold = SeverityLevel::CRITICAL;
    config.security.onUnsafe = UnsafeAction::ABORT;

    TestValidator validator(nullptr, config);
    ValidationFinding finding = make_finding("critical", 10.0);
    finding.kind = "vulnerability";
    finding.id = "GHSA-demo";
    validator.findings = {finding};

    Graph graph = make_mixed_graph();
    CHECK(validator.validate(&graph) == nullptr);
}

TEST_CASE("validator matches local OSV feed against graph packages", "[unit][validator][validate]") {
    TempDir tempDir{"reqpack-validator-osv"};
    const std::filesystem::path feedPath = tempDir.path() / "osv-feed.json";
    write_file(feedPath, R"([
        {
            "id": "CVE-2026-1",
            "modified": "2026-01-01T00:00:00Z",
            "summary": "ripgrep issue",
            "severity": [{"type": "CVSS_V3", "score": "9.8"}],
            "affected": [{
                "package": {"ecosystem": "Debian", "name": "ripgrep"},
                "versions": ["14.1"]
            }]
        }
    ])");

    ReqPackConfig config;
    config.security.osvDatabasePath = (tempDir.path() / "osv-db").string();
    config.security.osvFeedUrl = feedPath.string();
    config.security.osvRefreshMode = OsvRefreshMode::ALWAYS;
    config.security.osvEcosystemMap["dnf"] = "Debian";
    config.security.severityThreshold = SeverityLevel::HIGH;

    Validator validator(nullptr, config);
    Graph graph;
    boost::add_vertex(Package{.action = ActionType::INSTALL, .system = "dnf", .name = "ripgrep", .version = "14.1"}, graph);
    CHECK(validator.validate(&graph) == nullptr);
}

TEST_CASE("validator allows explicit update when target version is safe", "[unit][validator][validate]") {
    TempDir tempDir{"reqpack-validator-update-safe"};
    const std::filesystem::path feedPath = tempDir.path() / "osv-feed.json";
    write_file(feedPath, R"([
        {
            "id": "CVE-2026-1",
            "modified": "2026-01-01T00:00:00Z",
            "summary": "ripgrep issue",
            "severity": [{"type": "CVSS_V3", "score": "9.8"}],
            "affected": [{
                "package": {"ecosystem": "Debian", "name": "ripgrep"},
                "versions": ["14.1"]
            }]
        }
    ])");

    ReqPackConfig config;
    config.security.osvDatabasePath = (tempDir.path() / "osv-db").string();
    config.security.osvFeedUrl = feedPath.string();
    config.security.osvRefreshMode = OsvRefreshMode::ALWAYS;
    config.security.osvEcosystemMap["dnf"] = "Debian";
    config.security.severityThreshold = SeverityLevel::HIGH;
    config.security.onUnsafe = UnsafeAction::ABORT;

    Validator validator(nullptr, config);
    Graph graph;
    boost::add_vertex(Package{.action = ActionType::UPDATE, .system = "dnf", .name = "ripgrep", .version = "14.2"}, graph);
    CHECK(validator.validate(&graph) == &graph);
}

TEST_CASE("validator blocks explicit update when target version is unsafe", "[unit][validator][validate]") {
    TempDir tempDir{"reqpack-validator-update-unsafe"};
    const std::filesystem::path feedPath = tempDir.path() / "osv-feed.json";
    write_file(feedPath, R"([
        {
            "id": "CVE-2026-1",
            "modified": "2026-01-01T00:00:00Z",
            "summary": "ripgrep issue",
            "severity": [{"type": "CVSS_V3", "score": "9.8"}],
            "affected": [{
                "package": {"ecosystem": "Debian", "name": "ripgrep"},
                "versions": ["14.1"]
            }]
        }
    ])");

    ReqPackConfig config;
    config.security.osvDatabasePath = (tempDir.path() / "osv-db").string();
    config.security.osvFeedUrl = feedPath.string();
    config.security.osvRefreshMode = OsvRefreshMode::ALWAYS;
    config.security.osvEcosystemMap["dnf"] = "Debian";
    config.security.severityThreshold = SeverityLevel::HIGH;
    config.security.onUnsafe = UnsafeAction::ABORT;

    Validator validator(nullptr, config);
    Graph graph;
    boost::add_vertex(Package{.action = ActionType::UPDATE, .system = "dnf", .name = "ripgrep", .version = "14.1"}, graph);
    CHECK(validator.validate(&graph) == nullptr);
}

TEST_CASE("validator allows unversioned update because target version is unknown", "[unit][validator][validate]") {
    TempDir tempDir{"reqpack-validator-update-unversioned"};
    const std::filesystem::path feedPath = tempDir.path() / "osv-feed.json";
    write_file(feedPath, R"([
        {
            "id": "CVE-2026-1",
            "modified": "2026-01-01T00:00:00Z",
            "summary": "ripgrep issue",
            "severity": [{"type": "CVSS_V3", "score": "9.8"}],
            "affected": [{
                "package": {"ecosystem": "Debian", "name": "ripgrep"},
                "versions": ["14.1"]
            }]
        }
    ])");

    ReqPackConfig config;
    config.security.osvDatabasePath = (tempDir.path() / "osv-db").string();
    config.security.osvFeedUrl = feedPath.string();
    config.security.osvRefreshMode = OsvRefreshMode::ALWAYS;
    config.security.osvEcosystemMap["dnf"] = "Debian";
    config.security.severityThreshold = SeverityLevel::HIGH;
    config.security.onUnsafe = UnsafeAction::ABORT;

    Validator validator(nullptr, config);
    Graph graph;
    boost::add_vertex(Package{.action = ActionType::UPDATE, .system = "dnf", .name = "ripgrep"}, graph);
    CHECK(validator.validate(&graph) == &graph);
}

TEST_CASE("validator matches indexed advisory data without preloading whole database", "[unit][validator][validate]") {
    TempDir tempDir{"reqpack-validator-indexed"};

    ReqPackConfig config;
    config.security.osvDatabasePath = (tempDir.path() / "osv-db").string();
    config.security.osvRefreshMode = OsvRefreshMode::MANUAL;
    config.security.osvEcosystemMap["dnf"] = "Debian";
    config.security.severityThreshold = SeverityLevel::HIGH;

    VulnerabilityDatabase database(config);
    REQUIRE(database.ensureReady());

    OsvAdvisory advisory;
    advisory.id = "CVE-2026-indexed";
    advisory.modified = "2026-01-01T00:00:00Z";
    advisory.summary = "indexed ripgrep issue";
    advisory.rawJson = R"({
        "id": "CVE-2026-indexed",
        "modified": "2026-01-01T00:00:00Z",
        "summary": "indexed ripgrep issue",
        "severity": [{"type": "CVSS_V3", "score": "9.8"}],
        "affected": [{
            "package": {"ecosystem": "Debian", "name": "ripgrep"},
            "versions": ["14.1"]
        }]
    })";
    advisory.affected = {OsvAffectedPackage{.ecosystem = "Debian", .name = "ripgrep", .severity = "critical", .score = 9.8, .versions = {"14.1"}}};
    REQUIRE(database.upsertAdvisory(advisory));

    Validator validator(nullptr, config);
    Graph graph;
    boost::add_vertex(Package{.action = ActionType::INSTALL, .system = "dnf", .name = "ripgrep", .version = "14.1"}, graph);
    CHECK(validator.validate(&graph) == nullptr);
}

TEST_CASE("sync imports only advisories inside active ecosystem scope", "[unit][validator][validate]") {
    TempDir tempDir{"reqpack-validator-scope"};
    const std::filesystem::path feedPath = tempDir.path() / "osv-feed.json";
    write_file(feedPath, R"([
        {
            "id": "CVE-2026-debian",
            "modified": "2026-01-01T00:00:00Z",
            "affected": [{
                "package": {"ecosystem": "Debian", "name": "ripgrep"},
                "versions": ["14.1"]
            }]
        },
        {
            "id": "CVE-2026-rubygems",
            "modified": "2026-01-02T00:00:00Z",
            "affected": [{
                "package": {"ecosystem": "RubyGems", "name": "rails"},
                "versions": ["7.0.0"]
            }]
        }
    ])");

    ReqPackConfig config;
    config.security.osvDatabasePath = (tempDir.path() / "osv-db").string();
    config.security.osvFeedUrl = feedPath.string();
    config.security.osvRefreshMode = OsvRefreshMode::ALWAYS;

    StaticMetadataProvider metadataProvider;
    metadataProvider.metadata["dnf"].osvEcosystem = "Debian";

    VulnerabilityDatabase database(config);
    REQUIRE(database.ensureReady());
    VulnerabilitySyncService syncService(&database, &metadataProvider, config);

    CHECK(syncService.ensureReady().empty());

    const std::vector<std::string> debianIds = database.advisoryIdsForPackage("Debian", "ripgrep");
    REQUIRE(debianIds.size() == 1);
    CHECK(debianIds[0] == "CVE-2026-debian");
    CHECK(database.advisoryIdsForPackage("RubyGems", "rails").empty());
    CHECK(database.getSyncState("ecosystem_scope").value_or("") == "Debian");
}

TEST_CASE("scope change forces full resync with new ecosystem subset", "[unit][validator][validate]") {
    TempDir tempDir{"reqpack-validator-scope-change"};
    const std::filesystem::path feedPath = tempDir.path() / "osv-feed.json";
    write_file(feedPath, R"([
        {
            "id": "CVE-2026-debian",
            "modified": "2026-01-01T00:00:00Z",
            "affected": [{
                "package": {"ecosystem": "Debian", "name": "ripgrep"},
                "versions": ["14.1"]
            }]
        },
        {
            "id": "CVE-2026-rubygems",
            "modified": "2026-01-02T00:00:00Z",
            "affected": [{
                "package": {"ecosystem": "RubyGems", "name": "rails"},
                "versions": ["7.0.0"]
            }]
        }
    ])");

    ReqPackConfig config;
    config.security.osvDatabasePath = (tempDir.path() / "osv-db").string();
    config.security.osvFeedUrl = feedPath.string();
    config.security.osvRefreshMode = OsvRefreshMode::PERIODIC;
    config.security.osvRefreshIntervalSeconds = 24L * 60L * 60L;

    StaticMetadataProvider metadataProvider;
    metadataProvider.metadata["dnf"].osvEcosystem = "Debian";

    VulnerabilityDatabase database(config);
    REQUIRE(database.ensureReady());
    VulnerabilitySyncService syncService(&database, &metadataProvider, config);
    CHECK(syncService.ensureReady().empty());
    CHECK(database.advisoryIdsForPackage("Debian", "ripgrep").size() == 1);
    CHECK(database.advisoryIdsForPackage("RubyGems", "rails").empty());

    metadataProvider.metadata["bundler"].osvEcosystem = "RubyGems";
    CHECK(syncService.ensureReady().empty());

    const std::vector<std::string> debianIds = database.advisoryIdsForPackage("Debian", "ripgrep");
    REQUIRE(debianIds.size() == 1);
    CHECK(debianIds[0] == "CVE-2026-debian");

    const std::vector<std::string> rubygemsIds = database.advisoryIdsForPackage("RubyGems", "rails");
    REQUIRE(rubygemsIds.size() == 1);
    CHECK(rubygemsIds[0] == "CVE-2026-rubygems");
    CHECK(database.getSyncState("ecosystem_scope").value_or("") == "Debian\nRubyGems");
}

TEST_CASE("validator auto-fetches only required gateway ecosystem into per-ecosystem index", "[unit][validator][validate]") {
    TempDir tempDir{"reqpack-validator-gateway-auto-fetch"};
    const std::filesystem::path feedPath = tempDir.path() / "osv-feed.json";
    write_file(feedPath, R"([
        {
            "id": "CVE-2026-debian",
            "modified": "2026-01-01T00:00:00Z",
            "summary": "ripgrep issue",
            "severity": [{"type": "CVSS_V3", "score": "9.8"}],
            "affected": [{
                "package": {"ecosystem": "Debian", "name": "ripgrep"},
                "versions": ["14.1"]
            }]
        },
        {
            "id": "CVE-2026-rubygems",
            "modified": "2026-01-02T00:00:00Z",
            "affected": [{
                "package": {"ecosystem": "RubyGems", "name": "rails"},
                "versions": ["7.0.0"]
            }]
        }
    ])");

    ReqPackConfig config;
    config.security.autoFetch = true;
    config.security.indexPath = (tempDir.path() / "security-index").string();
    config.security.osvFeedUrl = feedPath.string();
    config.security.osvRefreshMode = OsvRefreshMode::MANUAL;
    config.security.gateways["security"].backends = {"osv"};

    StaticMetadataProvider metadataProvider;
    metadataProvider.metadata["dnf"].osvEcosystem = "Debian";

    Validator validator(&metadataProvider, config);
    Graph graph;
    boost::add_vertex(Package{.action = ActionType::INSTALL, .system = "dnf", .name = "ripgrep", .version = "14.1"}, graph);
    CHECK(validator.validate(&graph) == nullptr);

    ReqPackConfig debianConfig = config;
    debianConfig.security.osvDatabasePath = (tempDir.path() / "security-index" / "Debian").string();
    VulnerabilityDatabase debianDatabase(debianConfig);
    REQUIRE(debianDatabase.ensureReady());
    const std::vector<std::string> debianIds = debianDatabase.advisoryIdsForPackage("Debian", "ripgrep");
    REQUIRE(debianIds.size() == 1);
    CHECK(debianIds[0] == "CVE-2026-debian");

    const std::filesystem::path rubyIndexPath = tempDir.path() / "security-index" / "RubyGems";
    CHECK_FALSE(std::filesystem::exists(rubyIndexPath));
}

TEST_CASE("validator auto-fetches required ecosystem through default gateway", "[unit][validator][validate]") {
    TempDir tempDir{"reqpack-validator-default-gateway-auto-fetch"};
    const std::filesystem::path feedPath = tempDir.path() / "osv-feed.json";
    write_file(feedPath, R"([
        {
            "id": "CVE-2026-default-gateway",
            "modified": "2026-01-01T00:00:00Z",
            "summary": "ripgrep issue",
            "severity": [{"type": "CVSS_V3", "score": "9.8"}],
            "affected": [{
                "package": {"ecosystem": "Debian", "name": "ripgrep"},
                "versions": ["14.1"]
            }]
        },
        {
            "id": "CVE-2026-unused-ecosystem",
            "modified": "2026-01-02T00:00:00Z",
            "affected": [{
                "package": {"ecosystem": "RubyGems", "name": "rails"},
                "versions": ["7.0.0"]
            }]
        }
    ])");

    ReqPackConfig config;
    config.security.autoFetch = true;
    config.security.indexPath = (tempDir.path() / "security-index").string();
    config.security.osvFeedUrl = feedPath.string();
    config.security.osvRefreshMode = OsvRefreshMode::MANUAL;

    StaticMetadataProvider metadataProvider;
    metadataProvider.metadata["dnf"].osvEcosystem = "Debian";

    Validator validator(&metadataProvider, config);
    Graph graph;
    boost::add_vertex(Package{.action = ActionType::INSTALL, .system = "dnf", .name = "ripgrep", .version = "14.1"}, graph);
    CHECK(validator.validate(&graph) == nullptr);

    ReqPackConfig debianConfig = config;
    debianConfig.security.osvDatabasePath = (tempDir.path() / "security-index" / "Debian").string();
    VulnerabilityDatabase debianDatabase(debianConfig);
    REQUIRE(debianDatabase.ensureReady());
    const std::vector<std::string> debianIds = debianDatabase.advisoryIdsForPackage("Debian", "ripgrep");
    REQUIRE(debianIds.size() == 1);
    CHECK(debianIds[0] == "CVE-2026-default-gateway");

    const std::filesystem::path rubyIndexPath = tempDir.path() / "security-index" / "RubyGems";
    CHECK_FALSE(std::filesystem::exists(rubyIndexPath));
}

TEST_CASE("validator skips auto-fetch for unmapped systems", "[unit][validator][validate]") {
    TempDir tempDir{"reqpack-validator-unmapped-system"};

    ReqPackConfig config;
    config.security.autoFetch = true;
    config.security.indexPath = (tempDir.path() / "security-index").string();
    config.security.osvDatabasePath = (tempDir.path() / "osv-db").string();
    config.security.osvFeedUrl = (tempDir.path() / "missing-feed.json").string();
    config.security.osvRefreshMode = OsvRefreshMode::ALWAYS;
    config.security.gateways["security"].backends = {"osv"};

    StaticMetadataProvider metadataProvider;

    Validator validator(&metadataProvider, config);
    Graph graph;
    boost::add_vertex(Package{.action = ActionType::REMOVE, .system = "dnf", .name = "texlive-xurl"}, graph);

    CHECK(validator.validate(&graph) == &graph);
    const auto& findings = validator.getLastFindings();
    CHECK(std::none_of(findings.begin(), findings.end(), [](const ValidationFinding& finding) {
        return finding.kind == "sync_error";
    }));
    CHECK(std::any_of(findings.begin(), findings.end(), [](const ValidationFinding& finding) {
        return finding.kind == "unsupported_ecosystem" && finding.id == "dnf";
    }));
    CHECK_FALSE(std::filesystem::exists(tempDir.path() / "security-index" / "dnf"));
}

TEST_CASE("validator blocks vulnerable Maven package from scoped OSV index", "[unit][validator][validate]") {
    TempDir tempDir{"reqpack-validator-maven-log4j"};
    const std::filesystem::path feedPath = tempDir.path() / "osv-feed.json";
    write_file(feedPath, R"([
        {
            "id": "GHSA-jfh8-c2jp-5v3q",
            "modified": "2025-10-22T19:37:02Z",
            "summary": "Remote code injection in Log4j",
            "severity": [{"type": "CVSS_V3", "score": "CVSS:3.1/AV:N/AC:L/PR:N/UI:N/S:C/C:H/I:H/A:H/E:H"}],
            "database_specific": {"severity": "CRITICAL"},
            "affected": [{
                "package": {"ecosystem": "Maven", "name": "org.apache.logging.log4j:log4j-core"},
                "ranges": [{
                    "type": "ECOSYSTEM",
                    "events": [
                        {"introduced": "2.13.0"},
                        {"fixed": "2.15.0"}
                    ]
                }],
                "versions": ["2.13.0", "2.13.1", "2.13.2", "2.13.3", "2.14.0", "2.14.1"]
            }]
        }
    ])");

    ReqPackConfig config;
    config.security.autoFetch = true;
    config.security.indexPath = (tempDir.path() / "security-index").string();
    config.security.osvFeedUrl = feedPath.string();
    config.security.osvRefreshMode = OsvRefreshMode::MANUAL;
    config.security.gateways["security"].backends = {"osv"};
    config.security.severityThreshold = SeverityLevel::CRITICAL;
    config.security.onUnsafe = UnsafeAction::ABORT;

    StaticMetadataProvider metadataProvider;
    metadataProvider.metadata["maven"].osvEcosystem = "Maven";
    metadataProvider.metadata["maven"].purlType = "maven";
    metadataProvider.metadata["maven"].versionComparator.profile = "maven-comparable";

    Validator validator(&metadataProvider, config);
    Graph graph;
    boost::add_vertex(Package{
        .action = ActionType::INSTALL,
        .system = "maven",
        .name = "org.apache.logging.log4j:log4j-core",
        .version = "2.14.1"
    }, graph);

    CHECK(validator.validate(&graph) == nullptr);
    REQUIRE_FALSE(validator.getLastFindings().empty());
    CHECK(validator.getLastFindings().front().id == "GHSA-jfh8-c2jp-5v3q");
    CHECK(validator.getLastFindings().front().severity == "critical");
}
