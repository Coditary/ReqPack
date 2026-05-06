#include <catch2/catch.hpp>

#include <boost/graph/adjacency_list.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

#include "core/sbom_exporter.h"
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

Graph make_graph() {
    Graph graph;
    const auto react = boost::add_vertex(Package{.action = ActionType::SBOM, .system = "npm", .name = "react", .version = "18.3.1"}, graph);
    const auto scheduler = boost::add_vertex(Package{.action = ActionType::SBOM, .system = "npm", .name = "scheduler", .version = "0.24.0"}, graph);
    boost::add_edge(scheduler, react, graph);
    return graph;
}

}  // namespace

TEST_CASE("sbom exporter renders default table output", "[unit][sbom][export]") {
    ScopedColumnsEnv columns{"72"};
    ScopedEnvVar noColor{"NO_COLOR", "1"};
    ScopedEnvVar forceColor{"FORCE_COLOR", nullptr};
    SbomExporter exporter;
    Request request;
    request.action = ActionType::SBOM;

    Graph graph;
    boost::add_vertex(Package{.action = ActionType::SBOM, .system = "maven", .name = "org.apache.logging.log4j:log4j-core", .version = "2.13.1", .sourcePath = "/tmp/source/with many path parts/example artifact.jar"}, graph);

    const std::string rendered = exporter.renderGraph(graph, request);
    CHECK(rendered.find('\t') == std::string::npos);
    CHECK(rendered.find("SYSTEM NAME                         VERSION SOURCE") != std::string::npos);
    CHECK(rendered.find("maven  org.apache.logging.log4j:... 2.13.1  /tmp/source/with many path") != std::string::npos);
    CHECK(rendered.find(std::string{"\n"} + std::string(44, ' ') + "parts/example artifact.jar\n") != std::string::npos);
}

TEST_CASE("sbom exporter colorizes terminal table output", "[unit][sbom][export]") {
    ScopedColumnsEnv columns{"72"};
    ScopedEnvVar noColor{"NO_COLOR", nullptr};
    ScopedEnvVar forceColor{"FORCE_COLOR", "1"};
    SbomExporter exporter;
    Request request;
    request.action = ActionType::SBOM;

    const std::string rendered = exporter.renderGraph(make_graph(), request);
    CHECK(rendered.find("\033[1;31mnpm") != std::string::npos);
    CHECK(rendered.find("\033[90m-\033[0m") != std::string::npos);
}

TEST_CASE("sbom exporter supports no-wrap and wide table flags", "[unit][sbom][export]") {
    ScopedColumnsEnv columns{"72"};
    ScopedEnvVar noColor{"NO_COLOR", "1"};
    ScopedEnvVar forceColor{"FORCE_COLOR", nullptr};
    SbomExporter exporter;
    Request request;
    request.action = ActionType::SBOM;
    request.flags = {"wide", "no-wrap"};

    Graph graph;
    boost::add_vertex(Package{.action = ActionType::SBOM, .system = "maven", .name = "org.apache.logging.log4j:log4j-core", .version = "2.13.1", .sourcePath = "/tmp/source/with many path parts/example artifact.jar and more sections"}, graph);

    const std::string rendered = exporter.renderGraph(graph, request);
    CHECK(rendered.find("/tmp/source/with many path parts/example artifact.jar and more sections") != std::string::npos);
    CHECK(rendered.find(std::string{"\n"} + std::string(44, ' ') + "parts/example") == std::string::npos);
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

TEST_CASE("sbom exporter reports file-open failure through logger diagnostics", "[unit][sbom][export]") {
    TempDir tempDir{"reqpack-sbom-open-failure"};
    SbomExporter exporter;
    Request request;
    request.action = ActionType::SBOM;
    request.outputPath = tempDir.path().string();
    request.flags = {"force"};

    RecordingDisplay display;
    ScopedLoggerDisplay displayGuard(&display);

    CHECK_FALSE(exporter.exportGraph(make_graph(), request));
    Logger::instance().flushSync();
    CHECK(std::any_of(display.messages.begin(), display.messages.end(), [&](const std::string& message) {
        return message.find("failed to open sbom output path: " + tempDir.path().string()) != std::string::npos;
    }));
    CHECK(std::any_of(display.messages.begin(), display.messages.end(), [](const std::string& message) {
        return message.find("Cause: ReqPack could not open requested SBOM output file for writing.") != std::string::npos;
    }));
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
