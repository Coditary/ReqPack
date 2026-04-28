#include <catch2/catch.hpp>

#include <boost/graph/adjacency_list.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

#include "core/sbom_exporter.h"

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
    const auto react = boost::add_vertex(Package{.action = ActionType::SBOM, .system = "npm", .name = "react", .version = "18.3.1"}, graph);
    const auto scheduler = boost::add_vertex(Package{.action = ActionType::SBOM, .system = "npm", .name = "scheduler", .version = "0.24.0"}, graph);
    boost::add_edge(scheduler, react, graph);
    return graph;
}

}  // namespace

TEST_CASE("sbom exporter renders default table output", "[unit][sbom][export]") {
    SbomExporter exporter;
    Request request;
    request.action = ActionType::SBOM;

    const std::string rendered = exporter.renderGraph(make_graph(), request);
    CHECK(rendered.find("SYSTEM\tNAME\tVERSION\tSOURCE") != std::string::npos);
    CHECK(rendered.find("npm\treact\t18.3.1") != std::string::npos);
    CHECK(rendered.find("npm\tscheduler\t0.24.0") != std::string::npos);
}

TEST_CASE("sbom exporter renders raw json output", "[unit][sbom][export]") {
    SbomExporter exporter;
    Request request;
    request.action = ActionType::SBOM;
    request.outputFormat = "json";

    const std::string rendered = exporter.renderGraph(make_graph(), request);
    CHECK(rendered.find("\"packages\"") != std::string::npos);
    CHECK(rendered.find("\"dependencies\"") != std::string::npos);
    CHECK(rendered.find("\"name\": \"react\"") != std::string::npos);
}

TEST_CASE("sbom exporter defaults file output to cyclonedx json", "[unit][sbom][export]") {
    TempDir tempDir{"reqpack-sbom-export"};
    SbomExporter exporter;
    Request request;
    request.action = ActionType::SBOM;
    request.outputPath = (tempDir.path() / "sbom.json").string();

    REQUIRE(exporter.exportGraph(make_graph(), request));
    const std::string rendered = read_file(request.outputPath);
    CHECK(rendered.find("\"bomFormat\": \"CycloneDX\"") != std::string::npos);
    CHECK(rendered.find("\"components\"") != std::string::npos);
    CHECK(rendered.find("\"dependencies\"") != std::string::npos);
}

TEST_CASE("sbom exporter formats maven purls from plugin metadata", "[unit][sbom][export]") {
    StaticMetadataProvider metadataProvider;
    metadataProvider.metadata["maven"] = PluginSecurityMetadata{
        .osvEcosystem = "Maven",
        .purlType = "maven",
        .versionComparator = VersionComparatorSpec{.profile = "maven-comparable"},
    };

    SbomExporter exporter(&metadataProvider);
    Request request;
    request.action = ActionType::SBOM;
    request.outputFormat = "cyclonedx-json";

    Graph graph;
    boost::add_vertex(Package{.action = ActionType::SBOM, .system = "maven", .name = "org.slf4j:slf4j-api", .version = "2.0.16"}, graph);

    const std::string rendered = exporter.renderGraph(graph, request);
    CHECK(rendered.find("\"purl\": \"pkg:maven/org.slf4j/slf4j-api@2.0.16\"") != std::string::npos);
}

TEST_CASE("sbom exporter skips invalid maven purls", "[unit][sbom][export]") {
    StaticMetadataProvider metadataProvider;
    metadataProvider.metadata["maven"] = PluginSecurityMetadata{
        .osvEcosystem = "Maven",
        .purlType = "maven",
        .versionComparator = VersionComparatorSpec{.profile = "maven-comparable"},
    };

    SbomExporter exporter(&metadataProvider);
    Request request;
    request.action = ActionType::SBOM;
    request.outputFormat = "cyclonedx-json";

    Graph graph;
    boost::add_vertex(Package{.action = ActionType::SBOM, .system = "maven", .name = "slf4j-api", .version = "2.0.16"}, graph);

    const std::string rendered = exporter.renderGraph(graph, request);
    CHECK(rendered.find("\"purl\"") == std::string::npos);
}
