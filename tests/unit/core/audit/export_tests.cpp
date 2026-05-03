#include <catch2/catch.hpp>

#include <boost/graph/adjacency_list.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

#include "core/audit_exporter.h"

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

}  // namespace

TEST_CASE("audit exporter renders default table output", "[unit][audit][export]") {
    AuditExporter exporter;
    Request request;
    request.action = ActionType::AUDIT;

    const std::string rendered = exporter.renderGraph(make_graph(), make_findings(), request);
    CHECK(rendered.find("SYSTEM\tNAME\tVERSION\tFINDING\tSEVERITY\tSCORE\tMESSAGE") != std::string::npos);
    CHECK(rendered.find("npm\treact\t18.3.1\tCVE-2026-1234\thigh") != std::string::npos);
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
