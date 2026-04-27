#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>
#include <vector>

#include <boost/graph/graph_traits.hpp>

#include <catch2/catch.hpp>

#include "core/planner.h"

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

ReqPackConfig make_planner_test_config(const std::filesystem::path& root) {
    ReqPackConfig config;
    config.registry.pluginDirectory = (root / "plugins").string();
    config.registry.databasePath = (root / "registry-db").string();
    config.registry.autoLoadPlugins = true;
    config.registry.shutDownPluginsOnExit = true;
    config.planner.autoDownloadMissingPlugins = false;
    config.planner.autoDownloadMissingDependencies = false;
    return config;
}

std::filesystem::path add_plugin_script(const std::filesystem::path& pluginRoot, const std::string& pluginName, const std::string& content) {
    const std::filesystem::path scriptPath = pluginRoot / pluginName / (pluginName + ".lua");
    write_file(scriptPath, content);
    return scriptPath;
}

std::vector<Package> collect_packages(const Graph& graph) {
    std::vector<Package> packages;
    auto [vertex, vertexEnd] = boost::vertices(graph);
    for (; vertex != vertexEnd; ++vertex) {
        packages.push_back(graph[*vertex]);
    }
    return packages;
}

bool graph_contains_package(const Graph& graph, const std::string& system, const std::string& name) {
    auto [vertex, vertexEnd] = boost::vertices(graph);
    for (; vertex != vertexEnd; ++vertex) {
        const Package& package = graph[*vertex];
        if (package.system == system && package.name == name) {
            return true;
        }
    }
    return false;
}

std::optional<Graph::vertex_descriptor> find_package_vertex(const Graph& graph, const std::string& system, const std::string& name) {
    auto [vertex, vertexEnd] = boost::vertices(graph);
    for (; vertex != vertexEnd; ++vertex) {
        const Package& package = graph[*vertex];
        if (package.system == system && package.name == name) {
            return *vertex;
        }
    }
    return std::nullopt;
}

bool graph_has_dependency_edge(const Graph& graph, const std::string& fromSystem, const std::string& fromName, const std::string& toSystem, const std::string& toName) {
    const auto from = find_package_vertex(graph, fromSystem, fromName);
    const auto to = find_package_vertex(graph, toSystem, toName);
    if (!from.has_value() || !to.has_value()) {
        return false;
    }
    return boost::edge(from.value(), to.value(), graph).second;
}

const char* APP_PLUGIN = R"(
plugin = {}

function plugin.getName() return "app" end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements()
  return {
    { system = "dep", name = "runtime" },
  }
end
function plugin.getCategories() return { "pkg", "app" } end
function plugin.getMissingPackages(packages)
  local missing = {}
  for _, package in ipairs(packages) do
    missing[#missing + 1] = package
  end
  return missing
end
function plugin.install(context, packages) return true end
function plugin.installLocal(context, path) return true end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package) return { name = package, version = "1.0.0" } end
function plugin.shutdown() return true end
)";

const char* DEP_PLUGIN_MISSING = R"(
plugin = {}

function plugin.getName() return "dep" end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements()
  return {
    { system = "leaf", name = "openssl" },
  }
end
function plugin.getCategories() return { "pkg", "dep" } end
function plugin.getMissingPackages(packages)
  local missing = {}
  for _, package in ipairs(packages) do
    missing[#missing + 1] = package
  end
  return missing
end
function plugin.install(context, packages) return true end
function plugin.installLocal(context, path) return true end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package) return { name = package, version = "1.0.0" } end
function plugin.shutdown() return true end
)";

const char* DEP_PLUGIN_SATISFIED = R"(
plugin = {}

function plugin.getName() return "dep" end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "dep" } end
function plugin.getMissingPackages(packages)
  return {}
end
function plugin.install(context, packages) return true end
function plugin.installLocal(context, path) return true end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package) return { name = package, version = "1.0.0" } end
function plugin.shutdown() return true end
)";

const char* LEAF_PLUGIN = R"(
plugin = {}

function plugin.getName() return "leaf" end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "leaf" } end
function plugin.getMissingPackages(packages)
  local missing = {}
  for _, package in ipairs(packages) do
    missing[#missing + 1] = package
  end
  return missing
end
function plugin.install(context, packages) return true end
function plugin.installLocal(context, path) return true end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package) return { name = package, version = "1.0.0" } end
function plugin.shutdown() return true end
)";

}  // namespace

TEST_CASE("planner ensure builds dependency DAG from plugin requirements", "[integration][planner][service]") {
    TempDir tempDir{"reqpack-planner-ensure-dag"};
    ReqPackConfig config = make_planner_test_config(tempDir.path());

    add_plugin_script(tempDir.path() / "plugins", "app", APP_PLUGIN);
    add_plugin_script(tempDir.path() / "plugins", "dep", DEP_PLUGIN_MISSING);
    add_plugin_script(tempDir.path() / "plugins", "leaf", LEAF_PLUGIN);

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);
    Planner planner(&registry, registry.getDatabase(), config);

    std::unique_ptr<Graph> graph(planner.plan({Request{.action = ActionType::ENSURE, .system = "app"}}));
    REQUIRE(graph != nullptr);

    const std::vector<Package> packages = collect_packages(*graph);
    REQUIRE(packages.size() == 2);
    CHECK(graph_contains_package(*graph, "dep", "runtime"));
    CHECK(graph_contains_package(*graph, "leaf", "openssl"));
    CHECK(graph_has_dependency_edge(*graph, "leaf", "openssl", "dep", "runtime"));
}

TEST_CASE("planner ensure returns empty graph when plugin requirements are already satisfied", "[integration][planner][service]") {
    TempDir tempDir{"reqpack-planner-ensure-empty"};
    ReqPackConfig config = make_planner_test_config(tempDir.path());

    add_plugin_script(tempDir.path() / "plugins", "app", APP_PLUGIN);
    add_plugin_script(tempDir.path() / "plugins", "dep", DEP_PLUGIN_SATISFIED);

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);
    Planner planner(&registry, registry.getDatabase(), config);

    std::unique_ptr<Graph> graph(planner.plan({Request{.action = ActionType::ENSURE, .system = "app"}}));
    REQUIRE(graph != nullptr);
    CHECK(collect_packages(*graph).empty());
}

TEST_CASE("planner install marks requirements ready when plugin dependencies are already satisfied", "[integration][planner][service]") {
    TempDir tempDir{"reqpack-planner-marker"};
    ReqPackConfig config = make_planner_test_config(tempDir.path());

    add_plugin_script(tempDir.path() / "plugins", "app", APP_PLUGIN);
    add_plugin_script(tempDir.path() / "plugins", "dep", DEP_PLUGIN_SATISFIED);

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);
    Planner planner(&registry, registry.getDatabase(), config);

    std::unique_ptr<Graph> graph(planner.plan({Request{
        .action = ActionType::INSTALL,
        .system = "app",
        .packages = {"sample"},
    }}));
    REQUIRE(graph != nullptr);

    const std::vector<Package> packages = collect_packages(*graph);
    REQUIRE(packages.size() == 1);
    CHECK(packages[0].system == "app");
    CHECK(packages[0].name == "sample");
    CHECK(std::filesystem::exists(tempDir.path() / "plugins" / "app" / ".requirements_ready"));
}
