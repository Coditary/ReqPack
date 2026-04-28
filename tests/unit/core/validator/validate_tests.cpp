#include <catch2/catch.hpp>

#include <boost/graph/adjacency_list.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
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
    CHECK(database.getSyncState("ecosystem_scope").value_or({}) == "Debian");
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
    CHECK(database.getSyncState("ecosystem_scope").value_or({}) == "Debian\nRubyGems");
}
