#include <catch2/catch.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "core/download/downloader.h"
#include "core/registry/registry_database.h"
#include "core/registry/registry_database_core.h"
#include "core/download/downloader_core.h"

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

TEST_CASE("downloader public download copies local files and reports missing sources", "[unit][downloader][service]") {
    TempDir tempDir{"reqpack-downloader-public-download"};
    ReqPackConfig config = make_downloader_test_config(tempDir.path());
    Downloader downloader(nullptr, config);

    SECTION("existing local file is copied into target path") {
        const std::filesystem::path source = tempDir.path() / "source.lua";
        const std::filesystem::path target = tempDir.path() / "plugins" / "dnf" / "dnf.lua";
        write_file(source, "return { getName = function() return 'dnf' end }\n");

        REQUIRE(downloader.download(source.string(), target.string()));
        CHECK(read_file(target) == "return { getName = function() return 'dnf' end }\n");
    }

    SECTION("missing local file fails without creating target") {
        const std::filesystem::path target = tempDir.path() / "plugins" / "dnf" / "dnf.lua";

        CHECK_FALSE(downloader.download((tempDir.path() / "missing.lua").string(), target.string()));
        CHECK_FALSE(std::filesystem::exists(target));
    }
}

TEST_CASE("downloader rejects plugin download when disabled or database unavailable", "[unit][downloader][service]") {
    TempDir tempDir{"reqpack-downloader-guards"};

    SECTION("disabled downloader short-circuits before database access") {
        ReqPackConfig config = make_downloader_test_config(tempDir.path());
        config.downloader.enabled = false;

        CHECK_FALSE(Downloader(nullptr, config).downloadPlugin("dnf"));
    }

    SECTION("missing registry database blocks plugin download") {
        ReqPackConfig config = make_downloader_test_config(tempDir.path());

        CHECK_FALSE(Downloader(nullptr, config).downloadPlugin("dnf"));
    }
}

TEST_CASE("downloader resolves planner aliases before materializing plugins", "[unit][downloader][service]") {
    TempDir tempDir{"reqpack-downloader-planner-alias"};
    const std::filesystem::path source = tempDir.path() / "sources" / "dnf.lua";
    write_file(source, "return { getName = function() return 'dnf' end }\n");

    ReqPackConfig config = make_downloader_test_config(tempDir.path());
    config.planner.systemAliases["yum"] = "dnf";
    config.registry.sources["dnf"] = RegistrySourceEntry{
        .source = source.string(),
        .alias = false,
        .description = "dnf plugin",
    };

    RegistryDatabase database(config);
    REQUIRE(database.ensureReady());

    Downloader downloader(&database, config);
    REQUIRE(downloader.downloadPlugin("yum"));

    const std::filesystem::path dnfTarget = tempDir.path() / "plugins" / "dnf" / "dnf.lua";
    CHECK(std::filesystem::exists(dnfTarget));
    CHECK(read_file(dnfTarget) == "return { getName = function() return 'dnf' end }\n");
    CHECK_FALSE(std::filesystem::exists(tempDir.path() / "plugins" / "yum"));
}

TEST_CASE("downloader copies bundled plugin directories and skips git metadata", "[unit][downloader][service]") {
    TempDir tempDir{"reqpack-downloader-bundle-copy"};
    const std::filesystem::path bundleRoot = tempDir.path() / "bundle" / "dnf";
    write_file(bundleRoot / "dnf.lua", "return { getName = function() return 'dnf' end }\n");
    write_file(bundleRoot / "bootstrap.lua", "print('boot')\n");
    write_file(bundleRoot / "lib" / "helper.txt", "helper\n");
    write_file(bundleRoot / ".git" / "config", "ignored\n");

    ReqPackConfig config = make_downloader_test_config(tempDir.path());
    config.registry.sources["dnf"] = RegistrySourceEntry{
        .source = bundleRoot.string(),
        .alias = false,
        .description = "dnf bundle",
    };

    RegistryDatabase database(config);
    REQUIRE(database.ensureReady());

    const std::filesystem::path targetDirectory = tempDir.path() / "plugins" / "dnf";
    write_file(targetDirectory / "stale.txt", "stale\n");

    Downloader downloader(&database, config);
    REQUIRE(downloader.downloadPlugin("dnf"));

    CHECK_FALSE(std::filesystem::exists(targetDirectory / "stale.txt"));
    CHECK(read_file(targetDirectory / "dnf.lua") == "return { getName = function() return 'dnf' end }\n");
    CHECK(read_file(targetDirectory / "bootstrap.lua") == "print('boot')\n");
    CHECK(read_file(targetDirectory / "lib" / "helper.txt") == "helper\n");
    CHECK_FALSE(std::filesystem::exists(targetDirectory / ".git"));
}

TEST_CASE("downloader writes bootstrap scripts and removes stale bootstrap files", "[unit][downloader][service]") {
    TempDir tempDir{"reqpack-downloader-bootstrap"};

    SECTION("bootstrap from local source file is materialized next to plugin") {
        const std::filesystem::path sourceDirectory = tempDir.path() / "source-with-bootstrap";
        const std::filesystem::path source = sourceDirectory / "dnf.lua";
        write_file(source, "return { getName = function() return 'dnf' end }\n");
        write_file(sourceDirectory / "bootstrap.lua", "print('boot')\n");

        ReqPackConfig config = make_downloader_test_config(tempDir.path() / "with-bootstrap");
        config.registry.sources["dnf"] = RegistrySourceEntry{
            .source = source.string(),
            .alias = false,
            .description = "dnf plugin",
        };

        RegistryDatabase database(config);
        REQUIRE(database.ensureReady());
        REQUIRE(Downloader(&database, config).downloadPlugin("dnf"));

        const std::filesystem::path targetDirectory = std::filesystem::path(config.registry.pluginDirectory) / "dnf";
        CHECK(read_file(targetDirectory / "dnf.lua") == "return { getName = function() return 'dnf' end }\n");
        CHECK(read_file(targetDirectory / "bootstrap.lua") == "print('boot')\n");
    }

    SECTION("stale bootstrap is removed when source no longer ships one") {
        const std::filesystem::path source = tempDir.path() / "source-without-bootstrap" / "dnf.lua";
        write_file(source, "return { getName = function() return 'dnf' end }\n");

        ReqPackConfig config = make_downloader_test_config(tempDir.path() / "without-bootstrap");
        config.registry.sources["dnf"] = RegistrySourceEntry{
            .source = source.string(),
            .alias = false,
            .description = "dnf plugin",
        };

        RegistryDatabase database(config);
        REQUIRE(database.ensureReady());

        const std::filesystem::path targetDirectory = std::filesystem::path(config.registry.pluginDirectory) / "dnf";
        write_file(targetDirectory / "bootstrap.lua", "old\n");

        REQUIRE(Downloader(&database, config).downloadPlugin("dnf"));
        CHECK(read_file(targetDirectory / "dnf.lua") == "return { getName = function() return 'dnf' end }\n");
        CHECK_FALSE(std::filesystem::exists(targetDirectory / "bootstrap.lua"));
    }
}
