#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <catch2/catch.hpp>

#include "core/registry.h"

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

ReqPackConfig make_registry_test_config(const std::filesystem::path& root) {
    ReqPackConfig config;
    config.registry.pluginDirectory = (root / "plugins").string();
    config.registry.databasePath = (root / "registry-db").string();
    config.registry.autoLoadPlugins = true;
    config.registry.shutDownPluginsOnExit = true;
    return config;
}

std::filesystem::path add_plugin_script(const std::filesystem::path& pluginRoot, const std::string& pluginName, const std::string& content) {
    const std::filesystem::path scriptPath = pluginRoot / pluginName / (pluginName + ".lua");
    write_file(scriptPath, content);
    return scriptPath;
}

const char* VALID_PLUGIN = R"(
plugin = {}

function plugin.getName() return "valid-plugin" end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "test" } end
function plugin.getMissingPackages(packages) return packages end
function plugin.install(context, packages) return true end
function plugin.installLocal(context, path) return true end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package) return { name = package, version = "1", description = "ok" } end
function plugin.shutdown() return true end
)";

const char* INVALID_PLUGIN = R"(
plugin = {}

function plugin.getVersion() return "1.0.0" end
)";

}  // namespace

TEST_CASE("registry scans plugin directory and exposes registered plugin names", "[integration][registry][service]") {
    TempDir tempDir{"reqpack-registry-scan"};
    ReqPackConfig config = make_registry_test_config(tempDir.path());

    add_plugin_script(tempDir.path() / "plugins", "valid", VALID_PLUGIN);

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);

    const std::vector<std::string> names = registry.getAvailableNames();
    REQUIRE(names.size() == 1);
    CHECK(names[0] == "valid");
    CHECK(registry.getState("valid") == PluginState::REGISTERED);
    CHECK(registry.getPlugin("valid") != nullptr);
}

TEST_CASE("registry loads valid plugin and resolves category lookup", "[integration][registry][service]") {
    TempDir tempDir{"reqpack-registry-load"};
    ReqPackConfig config = make_registry_test_config(tempDir.path());

    add_plugin_script(tempDir.path() / "plugins", "valid", VALID_PLUGIN);

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);

    REQUIRE(registry.loadPlugin("valid"));
    CHECK(registry.isLoaded("valid"));
    CHECK(registry.getState("valid") == PluginState::ACTIVE);

    const std::vector<std::string> found = registry.findByCategory("test");
    REQUIRE(found.size() == 1);
    CHECK(found[0] == "valid");
}

TEST_CASE("registry marks invalid plugin as failed when contract validation fails", "[integration][registry][service]") {
    TempDir tempDir{"reqpack-registry-invalid"};
    ReqPackConfig config = make_registry_test_config(tempDir.path());

    add_plugin_script(tempDir.path() / "plugins", "broken", INVALID_PLUGIN);

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);

    CHECK_FALSE(registry.loadPlugin("broken"));
    CHECK_FALSE(registry.isLoaded("broken"));
    CHECK(registry.getState("broken") == PluginState::FAILED);
}

TEST_CASE("registry resolves aliases from database and config", "[integration][registry][service]") {
    TempDir tempDir{"reqpack-registry-alias"};
    ReqPackConfig config = make_registry_test_config(tempDir.path());
    config.planner.systemAliases["yum"] = "dnf";
    config.registry.sources["aliasdb"] = RegistrySourceEntry{.source = "valid", .alias = true, .description = "db alias"};

    add_plugin_script(tempDir.path() / "plugins", "valid", VALID_PLUGIN);

    Registry registry(config);
    REQUIRE(registry.getDatabase()->ensureReady());
    registry.scanDirectory(config.registry.pluginDirectory);

    CHECK(registry.resolvePluginName("ALIASDB") == "valid");
    CHECK(registry.resolvePluginName("YUM") == "dnf");
    CHECK(registry.resolvePluginName("VALID") == "valid");
}

TEST_CASE("registry materializes database-backed plugin script on load", "[integration][registry][service]") {
    TempDir tempDir{"reqpack-registry-materialize"};
    ReqPackConfig config = make_registry_test_config(tempDir.path());
    config.registry.sources["cached"] = RegistrySourceEntry{
        .source = (tempDir.path() / "remote-source" / "cached.lua").string(),
        .alias = false,
        .description = "cached plugin",
    };

    write_file(tempDir.path() / "remote-source" / "cached.lua", VALID_PLUGIN);

    Registry registry(config);
    REQUIRE(registry.getDatabase()->ensureReady());

    const std::filesystem::path materializedPath = tempDir.path() / "plugins" / "cached" / "cached.lua";
    CHECK_FALSE(std::filesystem::exists(materializedPath));

    REQUIRE(registry.loadPlugin("cached"));
    CHECK(std::filesystem::exists(materializedPath));
    CHECK(registry.getState("cached") == PluginState::ACTIVE);
    CHECK(registry.getPlugin("cached") != nullptr);
}
