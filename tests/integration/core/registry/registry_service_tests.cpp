#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <catch2/catch.hpp>

#include "core/registry/registry.h"
#include "core/registry/registry_database_core.h"

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

void init_git_repository(const std::filesystem::path& path) {
    REQUIRE(std::filesystem::exists(path.parent_path()));
    REQUIRE(std::system((std::string{"git init -q -b main \""} + path.string() + "\"").c_str()) == 0);
}

void commit_all_git_repository(const std::filesystem::path& path, const std::string& message) {
    REQUIRE(std::system((std::string{"git -C \""} + path.string() + "\" add .").c_str()) == 0);
    REQUIRE(std::system((std::string{"git -C \""} + path.string() +
                         "\" -c user.email=reqpack@test.invalid -c user.name=ReqPackTests commit -q -m \"" + message + "\"").c_str()) == 0);
}

void remove_git_path(const std::filesystem::path& repository, const std::filesystem::path& path) {
    REQUIRE(std::system((std::string{"git -C \""} + repository.string() + "\" rm -q \"" + path.string() + "\"").c_str()) == 0);
}

void move_git_path(const std::filesystem::path& repository, const std::filesystem::path& from, const std::filesystem::path& to) {
    REQUIRE(std::system((std::string{"git -C \""} + repository.string() + "\" mv \"" + from.string() + "\" \"" + to.string() + "\"").c_str()) == 0);
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
function plugin.getSecurityMetadata()
  return {
    role = "package-manager",
    capabilities = { "exec" },
    ecosystemScopes = { "demo-osv" },
    writeScopes = {
      { kind = "temp" },
    },
    networkScopes = {
      { host = "api.osv.dev", scheme = "https", pathPrefix = "/v1" },
    },
    privilegeLevel = "none",
  }
end
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

const char* MISMATCHED_METADATA_PLUGIN = R"(
plugin = {}

function plugin.getName() return "mismatch-plugin" end
function plugin.getVersion() return "1.0.0" end
function plugin.getSecurityMetadata()
  return {
    role = "package-manager",
    capabilities = { "exec" },
    ecosystemScopes = { "wrong-ecosystem" },
    writeScopes = {
      { kind = "plugin-data", value = "state" },
    },
    networkScopes = {
      { host = "mirror.example.test", scheme = "https", pathPrefix = "/plugins" },
    },
    privilegeLevel = "none",
  }
end
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

}  // namespace

TEST_CASE("registry scans plugin directory and exposes registered plugin names", "[integration][registry][service]") {
    TempDir tempDir{"reqpack-registry-scan"};
    ReqPackConfig config = make_registry_test_config(tempDir.path());

    add_plugin_script(tempDir.path() / "plugins", "valid", VALID_PLUGIN);

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);

    const std::vector<std::string> names = registry.getAvailableNames();
    REQUIRE(names.size() == 2);
    CHECK(std::find(names.begin(), names.end(), "rqp") != names.end());
    CHECK(std::find(names.begin(), names.end(), "valid") != names.end());
    CHECK(registry.getState("valid") == PluginState::REGISTERED);
    CHECK(registry.getPlugin("valid") != nullptr);
}

TEST_CASE("registry exposes built-in rqp and resolves rqp extension", "[integration][registry][service]") {
    TempDir tempDir{"reqpack-registry-built-in-rqp"};
    ReqPackConfig config = make_registry_test_config(tempDir.path());

    Registry registry(config);

    REQUIRE(registry.getPlugin("rqp") != nullptr);
    REQUIRE(registry.loadPlugin("rqp"));
    CHECK(registry.isLoaded("rqp"));
    CHECK(registry.resolveSystemForExtension(".rqp") == "rqp");
}

TEST_CASE("registry ignores external rqp plugin override", "[integration][registry][service]") {
    TempDir tempDir{"reqpack-registry-rqp-override"};
    ReqPackConfig config = make_registry_test_config(tempDir.path());

    add_plugin_script(tempDir.path() / "plugins", "rqp", VALID_PLUGIN);

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);

    REQUIRE(registry.getPlugin("rqp") != nullptr);
    REQUIRE(registry.loadPlugin("rqp"));
    CHECK(registry.resolveSystemForExtension(".rqp") == "rqp");
    CHECK(registry.getPlugin("rqp")->getName() == "ReqPack Native Package Manager");
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
        .role = "package-manager",
        .capabilities = {"exec"},
        .ecosystemScopes = {"demo-osv"},
        .writeScopes = {{.kind = "temp", .value = {}}},
        .networkScopes = {{.host = "api.osv.dev", .scheme = "https", .pathPrefix = "/v1"}},
        .privilegeLevel = "none",
        .scriptSha256 = registry_database_sha256_hex(VALID_PLUGIN),
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

TEST_CASE("registry blocks database-backed plugin load when thin-layer metadata is required but missing", "[integration][registry][service]") {
    TempDir tempDir{"reqpack-registry-thin-layer-block"};
    ReqPackConfig config = make_registry_test_config(tempDir.path());
    config.security.requireThinLayer = true;
    config.registry.sources["cached"] = RegistrySourceEntry{
        .source = (tempDir.path() / "remote-source" / "cached.lua").string(),
        .alias = false,
        .description = "cached plugin",
    };

    write_file(tempDir.path() / "remote-source" / "cached.lua", VALID_PLUGIN);

    Registry registry(config);
    REQUIRE(registry.getDatabase()->ensureReady());

    CHECK_FALSE(registry.loadPlugin("cached"));
    CHECK(registry.getState("cached") == PluginState::FAILED);
    CHECK_FALSE(std::filesystem::exists(tempDir.path() / "plugins" / "cached" / "cached.lua"));
}

TEST_CASE("registry allows database-backed plugin load when thin-layer metadata is present", "[integration][registry][service]") {
    TempDir tempDir{"reqpack-registry-thin-layer-pass"};
    ReqPackConfig config = make_registry_test_config(tempDir.path());
    config.security.requireThinLayer = true;
    config.registry.sources["cached"] = RegistrySourceEntry{
        .source = (tempDir.path() / "remote-source" / "cached.lua").string(),
        .alias = false,
        .description = "cached plugin",
        .role = "package-manager",
        .capabilities = {"exec"},
        .ecosystemScopes = {"demo-osv"},
        .writeScopes = {{.kind = "temp", .value = {}}},
        .networkScopes = {{.host = "api.osv.dev", .scheme = "https", .pathPrefix = "/v1"}},
        .privilegeLevel = "none",
        .scriptSha256 = registry_database_sha256_hex(VALID_PLUGIN),
    };

    write_file(tempDir.path() / "remote-source" / "cached.lua", VALID_PLUGIN);

    Registry registry(config);
    REQUIRE(registry.getDatabase()->ensureReady());

    REQUIRE(registry.loadPlugin("cached"));
    CHECK(registry.getState("cached") == PluginState::ACTIVE);
    CHECK(std::filesystem::exists(tempDir.path() / "plugins" / "cached" / "cached.lua"));
}

TEST_CASE("registry blocks unpinned git-backed plugin load when thin-layer trust is required", "[integration][registry][service]") {
    TempDir tempDir{"reqpack-registry-thin-layer-git-ref-block"};
    ReqPackConfig config = make_registry_test_config(tempDir.path());
    config.security.requireThinLayer = true;
    config.registry.sources["cached"] = RegistrySourceEntry{
        .source = "git+https://example.test/plugins/cached.git",
        .alias = false,
        .description = "cached plugin",
        .role = "package-manager",
        .capabilities = {"exec"},
        .privilegeLevel = "none",
    };

    Registry registry(config);
    REQUIRE(registry.getDatabase()->ensureReady());

    CHECK_FALSE(registry.loadPlugin("cached"));
    CHECK(registry.getState("cached") == PluginState::FAILED);
    CHECK_FALSE(std::filesystem::exists(tempDir.path() / "plugins" / "cached" / "cached.lua"));
}

TEST_CASE("registry blocks database-backed plugin load when thin-layer script hash mismatches", "[integration][registry][service]") {
    TempDir tempDir{"reqpack-registry-thin-layer-hash-block"};
    ReqPackConfig config = make_registry_test_config(tempDir.path());
    config.security.requireThinLayer = true;
    config.registry.sources["cached"] = RegistrySourceEntry{
        .source = (tempDir.path() / "remote-source" / "cached.lua").string(),
        .alias = false,
        .description = "cached plugin",
        .role = "package-manager",
        .capabilities = {"exec"},
        .ecosystemScopes = {"demo-osv"},
        .writeScopes = {{.kind = "temp", .value = {}}},
        .networkScopes = {{.host = "api.osv.dev", .scheme = "https", .pathPrefix = "/v1"}},
        .privilegeLevel = "none",
        .scriptSha256 = std::string(64, '0'),
    };

    write_file(tempDir.path() / "remote-source" / "cached.lua", VALID_PLUGIN);

    Registry registry(config);
    REQUIRE(registry.getDatabase()->ensureReady());

    CHECK_FALSE(registry.loadPlugin("cached"));
    CHECK(registry.getState("cached") == PluginState::FAILED);
    CHECK_FALSE(std::filesystem::exists(tempDir.path() / "plugins" / "cached" / "cached.lua"));
}

TEST_CASE("registry blocks database-backed plugin load when runtime metadata mismatches trust record", "[integration][registry][service]") {
    TempDir tempDir{"reqpack-registry-thin-layer-runtime-mismatch"};
    ReqPackConfig config = make_registry_test_config(tempDir.path());
    config.security.requireThinLayer = true;
    config.registry.sources["cached"] = RegistrySourceEntry{
        .source = (tempDir.path() / "remote-source" / "cached.lua").string(),
        .alias = false,
        .description = "cached plugin",
        .role = "package-manager",
        .capabilities = {"exec"},
        .ecosystemScopes = {"demo-osv"},
        .writeScopes = {{.kind = "temp", .value = {}}},
        .networkScopes = {{.host = "api.osv.dev", .scheme = "https", .pathPrefix = "/v1"}},
        .privilegeLevel = "none",
        .scriptSha256 = registry_database_sha256_hex(MISMATCHED_METADATA_PLUGIN),
    };

    write_file(tempDir.path() / "remote-source" / "cached.lua", MISMATCHED_METADATA_PLUGIN);

    Registry registry(config);
    REQUIRE(registry.getDatabase()->ensureReady());

    CHECK_FALSE(registry.loadPlugin("cached"));
    CHECK(registry.getState("cached") == PluginState::FAILED);
}

TEST_CASE("registry bootstrap scripts may use io library", "[integration][registry][service]") {
    TempDir tempDir{"reqpack-registry-bootstrap-io"};
    ReqPackConfig config = make_registry_test_config(tempDir.path());

    add_plugin_script(tempDir.path() / "plugins", "valid", VALID_PLUGIN);
    write_file(tempDir.path() / "plugins" / "valid" / "bootstrap.lua",
        "function bootstrap()\n"
        "  local marker = io.open(REQPACK_PLUGIN_DIR .. '/bootstrapped.txt', 'w')\n"
        "  if marker ~= nil then\n"
        "    marker:write('ok\\n')\n"
        "    marker:close()\n"
        "  end\n"
        "  return true\n"
        "end\n");

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);

    REQUIRE(registry.loadPlugin("valid"));
    CHECK(std::filesystem::exists(tempDir.path() / "plugins" / "valid" / "bootstrapped.txt"));
}

TEST_CASE("registry bootstraps metadata from git json registry and lazily materializes payload", "[integration][registry][service]") {
    TempDir tempDir{"reqpack-registry-json-bootstrap"};
    ReqPackConfig config = make_registry_test_config(tempDir.path());
    const std::filesystem::path remoteRegistry = tempDir.path() / "remote-registry";
    const std::filesystem::path remotePlugin = tempDir.path() / "plugin-source";

    std::filesystem::create_directories(tempDir.path());
    std::filesystem::create_directories(remotePlugin);
    init_git_repository(remoteRegistry);
    init_git_repository(remotePlugin);

    write_file(remotePlugin / "valid.lua", VALID_PLUGIN);
    commit_all_git_repository(remotePlugin, "plugin");

    const std::string registryJson =
        std::string{"{\n"}
        + "  \"schemaVersion\": 1,\n"
        + "  \"name\": \"valid\",\n"
        + "  \"source\": \"git+" + remotePlugin.string() + "?ref=main\",\n"
        + "  \"description\": \"valid plugin\",\n"
        + "  \"role\": \"package-manager\",\n"
        + "  \"capabilities\": [\"exec\"],\n"
        + "  \"ecosystemScopes\": [\"demo-osv\"],\n"
        + "  \"writeScopes\": [{\"kind\": \"temp\"}],\n"
        + "  \"networkScopes\": [{\"host\": \"api.osv.dev\", \"scheme\": \"https\", \"pathPrefix\": \"/v1\"}],\n"
        + "  \"privilegeLevel\": \"none\",\n"
        + "  \"scriptSha256\": \"" + registry_database_sha256_hex(VALID_PLUGIN) + "\",\n"
        + "  \"aliases\": [{\"name\": \"okay\"}]\n"
        + "}\n";
    write_file(remoteRegistry / "registry" / "v" / "valid.json", registryJson);
    commit_all_git_repository(remoteRegistry, "registry");

    config.registry.remoteUrl = std::string{"git+"} + remoteRegistry.string();
    config.registry.remoteBranch = "main";
    config.registry.remotePluginsPath = "registry";

    Registry registry(config);
    REQUIRE(registry.getDatabase()->ensureReady());

    const std::optional<RegistryRecord> seeded = registry.getDatabase()->getRecord("valid");
    REQUIRE(seeded.has_value());
    CHECK(seeded->originPath.find("registry/v/valid.json") != std::string::npos);
    CHECK(seeded->script.empty());
    REQUIRE(registry.getDatabase()->getRecord("okay").has_value());
    CHECK(registry.getDatabase()->getMetaValue("remotePluginsPath").value() == "registry");

    REQUIRE(registry.loadPlugin("okay"));
    CHECK(std::filesystem::exists(tempDir.path() / "plugins" / "valid" / "valid.lua"));

    const std::optional<RegistryRecord> refreshed = registry.getDatabase()->getRecord("valid");
    REQUIRE(refreshed.has_value());
    CHECK_FALSE(refreshed->script.empty());
}

TEST_CASE("registry layers explicit config sources on top of git json main registry", "[integration][registry][service]") {
    TempDir tempDir{"reqpack-registry-json-explicit"};
    ReqPackConfig config = make_registry_test_config(tempDir.path());
    const std::filesystem::path remoteRegistry = tempDir.path() / "remote-registry";
    const std::filesystem::path remotePlugin = tempDir.path() / "plugin-source";
    const std::filesystem::path explicitSource = tempDir.path() / "explicit-source" / "cached.lua";

    std::filesystem::create_directories(tempDir.path());
    std::filesystem::create_directories(remotePlugin);
    init_git_repository(remoteRegistry);
    init_git_repository(remotePlugin);

    write_file(remotePlugin / "valid.lua", VALID_PLUGIN);
    commit_all_git_repository(remotePlugin, "plugin");
    write_file(explicitSource, VALID_PLUGIN);

    write_file(remoteRegistry / "registry" / "v" / "valid.json", std::string{
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"name\": \"valid\",\n"
        "  \"source\": \"git+" + remotePlugin.string() + "?ref=main\",\n"
        "  \"description\": \"valid plugin\",\n"
        "  \"role\": \"package-manager\",\n"
        "  \"privilegeLevel\": \"none\"\n"
        "}\n"
    });
    commit_all_git_repository(remoteRegistry, "registry");

    config.registry.remoteUrl = std::string{"git+"} + remoteRegistry.string();
    config.registry.remoteBranch = "main";
    config.registry.remotePluginsPath = "registry";
    config.registry.sources["cached"] = RegistrySourceEntry{
        .source = explicitSource.string(),
        .alias = false,
        .description = "cached plugin",
    };

    RegistryDatabase database(config);
    REQUIRE(database.ensureReady());
    REQUIRE(database.getRecord("valid").has_value());
    REQUIRE(database.getRecord("cached").has_value());
    CHECK(database.getRecord("cached")->source == explicitSource.string());
}

TEST_CASE("registry bootstrap ignores legacy lua registry files but keeps explicit config sources", "[integration][registry][service]") {
    TempDir tempDir{"reqpack-registry-legacy-bootstrap-ignore"};
    ReqPackConfig config = make_registry_test_config(tempDir.path());
    const std::filesystem::path explicitSource = tempDir.path() / "explicit-source" / "apt.lua";
    const std::filesystem::path overlayPath = tempDir.path() / "overlay.lua";

    write_file(registry_source_file_path(config.registry.databasePath), R"(
        return {
            sources = {
                DNF = "https://legacy.test/dnf.lua",
                Yum = {
                    alias = true,
                    source = "DNF",
                },
            },
        }
    )");
    write_file(overlayPath, R"(
        return {
            sources = {
                Brew = "https://overlay.test/brew.lua",
            },
        }
    )");
    write_file(explicitSource, VALID_PLUGIN);

    config.registry.overlayPath = overlayPath.string();
    config.registry.sources["apt"] = RegistrySourceEntry{
        .source = explicitSource.string(),
        .alias = false,
        .description = "explicit plugin",
    };

    RegistryDatabase database(config);
    REQUIRE(database.ensureReady());
    REQUIRE(database.getRecord("apt").has_value());
    CHECK_FALSE(database.getRecord("dnf").has_value());
    CHECK_FALSE(database.getRecord("yum").has_value());
    CHECK_FALSE(database.getRecord("brew").has_value());
}

TEST_CASE("registry git json delta sync updates and deletes touched files only", "[integration][registry][service]") {
    TempDir tempDir{"reqpack-registry-json-delta"};
    ReqPackConfig config = make_registry_test_config(tempDir.path());
    const std::filesystem::path remoteRegistry = tempDir.path() / "remote-registry";

    init_git_repository(remoteRegistry);
    write_file(remoteRegistry / "registry" / "d" / "dnf.json", R"({
  "schemaVersion": 1,
  "name": "dnf",
  "source": "git+https://example.test/dnf.git?ref=v1",
  "description": "dnf v1",
  "role": "package-manager",
  "privilegeLevel": "none",
  "aliases": [{"name": "yum"}]
})");
    write_file(remoteRegistry / "registry" / "m" / "maven.json", R"({
  "schemaVersion": 1,
  "name": "maven",
  "source": "git+https://example.test/maven.git?ref=v1",
  "description": "maven v1",
  "role": "package-manager",
  "privilegeLevel": "none"
})");
    commit_all_git_repository(remoteRegistry, "initial");

    config.registry.remoteUrl = std::string{"git+"} + remoteRegistry.string();
    config.registry.remoteBranch = "main";
    config.registry.remotePluginsPath = "registry";

    RegistryDatabase database(config);
    REQUIRE(database.ensureReady());
    REQUIRE(database.getRecord("dnf").has_value());
    REQUIRE(database.getRecord("yum").has_value());
    REQUIRE(database.getRecord("maven").has_value());
    const std::string firstCommit = database.getMetaValue("lastCommit").value();

    write_file(remoteRegistry / "registry" / "d" / "dnf.json", R"({
  "schemaVersion": 1,
  "name": "dnf",
  "source": "git+https://example.test/dnf.git?ref=v2",
  "description": "dnf v2",
  "role": "package-manager",
  "privilegeLevel": "sudo",
  "aliases": [{"name": "dnf5"}]
})");
    remove_git_path(remoteRegistry, remoteRegistry / "registry" / "m" / "maven.json");
    commit_all_git_repository(remoteRegistry, "delta");

    RegistryDatabase reopened(config);
    REQUIRE(reopened.ensureReady());
    REQUIRE(reopened.getRecord("dnf").has_value());
    CHECK(reopened.getRecord("dnf")->description == "dnf v2");
    CHECK(reopened.getRecord("dnf")->privilegeLevel == "sudo");
    CHECK_FALSE(reopened.getRecord("yum").has_value());
    REQUIRE(reopened.getRecord("dnf5").has_value());
    CHECK_FALSE(reopened.getRecord("maven").has_value());
    REQUIRE(reopened.getMetaValue("lastCommit").has_value());
    CHECK(reopened.getMetaValue("lastCommit").value() != firstCommit);
}

TEST_CASE("registry git json delta keeps previous state on invalid changed file", "[integration][registry][service]") {
    TempDir tempDir{"reqpack-registry-json-delta-invalid"};
    ReqPackConfig config = make_registry_test_config(tempDir.path());
    const std::filesystem::path remoteRegistry = tempDir.path() / "remote-registry";

    init_git_repository(remoteRegistry);
    write_file(remoteRegistry / "registry" / "d" / "dnf.json", R"({
  "schemaVersion": 1,
  "name": "dnf",
  "source": "git+https://example.test/dnf.git?ref=v1",
  "description": "dnf ok",
  "role": "package-manager",
  "privilegeLevel": "none",
  "aliases": [{"name": "yum"}]
})");
    commit_all_git_repository(remoteRegistry, "initial");

    config.registry.remoteUrl = std::string{"git+"} + remoteRegistry.string();
    config.registry.remoteBranch = "main";
    config.registry.remotePluginsPath = "registry";

    RegistryDatabase database(config);
    REQUIRE(database.ensureReady());
    const std::string firstCommit = database.getMetaValue("lastCommit").value();
    REQUIRE(database.getRecord("yum").has_value());

    write_file(remoteRegistry / "registry" / "d" / "dnf.json", R"({
  "schemaVersion": 1,
  "name": "broken"
})");
    commit_all_git_repository(remoteRegistry, "broken");

    RegistryDatabase reopened(config);
    REQUIRE(reopened.ensureReady());
    REQUIRE(reopened.getRecord("dnf").has_value());
    CHECK(reopened.getRecord("dnf")->description == "dnf ok");
    REQUIRE(reopened.getRecord("yum").has_value());
    REQUIRE(reopened.getMetaValue("lastCommit").has_value());
    CHECK(reopened.getMetaValue("lastCommit").value() == firstCommit);
}
