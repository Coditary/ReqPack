#include <catch2/catch.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "core/downloader.h"
#include "core/registry_database.h"
#include "core/registry_database_core.h"
#include "core/downloader_core.h"

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

bool copy_local_source_to_target(const std::string& source, const std::filesystem::path& targetPath) {
    std::filesystem::create_directories(targetPath.parent_path());
    if (downloader_is_remote_source(source)) {
        return false;
    }

    std::error_code error;
    std::filesystem::copy_file(source, targetPath, std::filesystem::copy_options::overwrite_existing, error);
    return !error;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.is_open());
    output << content;
}

ReqPackConfig make_downloader_test_config(const std::filesystem::path& root) {
    ReqPackConfig config;
    config.registry.pluginDirectory = (root / "plugins").string();
    config.registry.databasePath = (root / "registry-db").string();
    config.downloader.enabled = true;
    return config;
}

}  // namespace

TEST_CASE("downloader validates plugin payload and rejects HTML or empty content", "[unit][downloader][payload]") {
    CHECK(downloader_is_valid_plugin_script("return { getName = function() return 'x' end }"));
    CHECK_FALSE(downloader_is_valid_plugin_script("   \n\t  "));
    CHECK_FALSE(downloader_is_valid_plugin_script("<!DOCTYPE html><html><body>forbidden</body></html>"));
    CHECK_FALSE(downloader_is_valid_plugin_script("<html><head></head><body>forbidden</body></html>"));
}

TEST_CASE("downloader distinguishes local and remote sources", "[unit][downloader][source]") {
    CHECK_FALSE(downloader_is_remote_source("/tmp/plugin.lua"));
    CHECK_FALSE(downloader_is_remote_source("relative/plugin.lua"));
    CHECK(downloader_is_remote_source("https://example.test/plugin.lua"));
    CHECK(downloader_is_remote_source("http://example.test/plugin.lua"));
}

TEST_CASE("downloader temp path and plugin target path are derived deterministically", "[unit][downloader][path]") {
    CHECK(downloader_temp_path_for_target("/tmp/plugin.lua") == std::filesystem::path("/tmp/plugin.lua.tmp"));

    ReqPackConfig config;
    config.registry.pluginDirectory = "/tmp/plugins";
    CHECK(downloader_plugin_target_path(config, "dnf") == std::filesystem::path("/tmp/plugins/dnf/dnf.lua"));
}

TEST_CASE("downloader local copy branch succeeds for existing file", "[unit][downloader][path]") {
    TempDir tempDir{"reqpack-downloader-copy-ok"};
    const std::filesystem::path source = tempDir.path() / "source.lua";
    const std::filesystem::path target = tempDir.path() / "plugins/dnf/dnf.lua";
    write_file(source, "return {}\n");

    REQUIRE(copy_local_source_to_target(source.string(), target));
    CHECK(std::filesystem::exists(target));
    CHECK(read_file(target) == "return {}\n");
}

TEST_CASE("downloader local copy branch fails for missing file", "[unit][downloader][path]") {
    TempDir tempDir{"reqpack-downloader-copy-fail"};
    const std::filesystem::path missing = tempDir.path() / "missing.lua";
    const std::filesystem::path target = tempDir.path() / "plugins/dnf/dnf.lua";

    CHECK_FALSE(copy_local_source_to_target(missing.string(), target));
    CHECK_FALSE(std::filesystem::exists(target));
}

TEST_CASE("downloader materializes cached registry plugin when thin-layer metadata passes", "[unit][downloader][service]") {
    TempDir tempDir{"reqpack-downloader-trust-pass"};
    const std::filesystem::path source = tempDir.path() / "remote-source" / "dnf.lua";
    const std::filesystem::path target = tempDir.path() / "plugins" / "dnf" / "dnf.lua";
    write_file(source, "return { getName = function() return 'dnf' end }\n");

    ReqPackConfig config = make_downloader_test_config(tempDir.path());
    config.security.requireThinLayer = true;
    config.registry.sources["dnf"] = RegistrySourceEntry{
        .source = source.string(),
        .alias = false,
        .description = "dnf plugin",
        .role = "package-manager",
        .capabilities = {"exec"},
        .privilegeLevel = "none",
        .scriptSha256 = registry_database_sha256_hex("return { getName = function() return 'dnf' end }\n"),
    };

    RegistryDatabase database(config);
    REQUIRE(database.ensureReady());

    Downloader downloader(&database, config);
    REQUIRE(downloader.downloadPlugin("dnf"));
    CHECK(std::filesystem::exists(target));
    CHECK(read_file(target) == "return { getName = function() return 'dnf' end }\n");
}

TEST_CASE("downloader blocks registry plugin materialization when thin-layer metadata is missing", "[unit][downloader][service]") {
    TempDir tempDir{"reqpack-downloader-trust-block"};
    const std::filesystem::path source = tempDir.path() / "remote-source" / "dnf.lua";
    const std::filesystem::path target = tempDir.path() / "plugins" / "dnf" / "dnf.lua";
    write_file(source, "return { getName = function() return 'dnf' end }\n");

    ReqPackConfig config = make_downloader_test_config(tempDir.path());
    config.security.requireThinLayer = true;
    config.registry.sources["dnf"] = RegistrySourceEntry{
        .source = source.string(),
        .alias = false,
        .description = "dnf plugin",
    };

    RegistryDatabase database(config);
    REQUIRE(database.ensureReady());

    Downloader downloader(&database, config);
    CHECK_FALSE(downloader.downloadPlugin("dnf"));
    CHECK_FALSE(std::filesystem::exists(target));
}

TEST_CASE("downloader blocks registry plugin materialization when script hash mismatches", "[unit][downloader][service]") {
    TempDir tempDir{"reqpack-downloader-hash-block"};
    const std::filesystem::path source = tempDir.path() / "remote-source" / "dnf.lua";
    const std::filesystem::path target = tempDir.path() / "plugins" / "dnf" / "dnf.lua";
    write_file(source, "return { getName = function() return 'dnf' end }\n");

    ReqPackConfig config = make_downloader_test_config(tempDir.path());
    config.security.requireThinLayer = true;
    config.registry.sources["dnf"] = RegistrySourceEntry{
        .source = source.string(),
        .alias = false,
        .description = "dnf plugin",
        .role = "package-manager",
        .capabilities = {"exec"},
        .privilegeLevel = "none",
        .scriptSha256 = std::string(64, '0'),
    };

    RegistryDatabase database(config);
    REQUIRE(database.ensureReady());

    Downloader downloader(&database, config);
    CHECK_FALSE(downloader.downloadPlugin("dnf"));
    CHECK_FALSE(std::filesystem::exists(target));
}

TEST_CASE("downloader blocks unpinned git registry plugin when thin-layer trust is required", "[unit][downloader][service]") {
    TempDir tempDir{"reqpack-downloader-git-unpinned-block"};
    const std::filesystem::path target = tempDir.path() / "plugins" / "dnf" / "dnf.lua";

    ReqPackConfig config = make_downloader_test_config(tempDir.path());
    config.security.requireThinLayer = true;
    config.registry.sources["dnf"] = RegistrySourceEntry{
        .source = "git+https://example.test/plugins/dnf.git",
        .alias = false,
        .description = "dnf plugin",
        .role = "package-manager",
        .capabilities = {"exec"},
        .privilegeLevel = "none",
    };

    RegistryDatabase database(config);
    REQUIRE(database.ensureReady());

    Downloader downloader(&database, config);
    CHECK_FALSE(downloader.downloadPlugin("dnf"));
    CHECK_FALSE(std::filesystem::exists(target));
}
