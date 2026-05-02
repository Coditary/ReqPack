#include <catch2/catch.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "core/configuration.h"
#include "core/remote_profiles.h"

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
    std::ofstream output(path);
    REQUIRE(output.is_open());
    output << content;
    output.close();
}

std::filesystem::path reqpack_user_home() {
    return reqpack_home_directory().parent_path();
}

}  // namespace

TEST_CASE("configuration parses known enum strings and rejects invalid values", "[unit][configuration][parse]") {
    REQUIRE(severity_level_from_string("HiGh").has_value());
    CHECK(severity_level_from_string("HiGh").value() == SeverityLevel::HIGH);
    CHECK_FALSE(severity_level_from_string("urgent").has_value());

    REQUIRE(log_level_from_string("warning").has_value());
    CHECK(log_level_from_string("warning").value() == LogLevel::WARN);
    CHECK_FALSE(log_level_from_string("loud").has_value());

    REQUIRE(report_format_from_string("JSON").has_value());
    CHECK(report_format_from_string("JSON").value() == ReportFormat::JSON);
    CHECK_FALSE(report_format_from_string("xml").has_value());

    REQUIRE(unsafe_action_from_string("ask").has_value());
    CHECK(unsafe_action_from_string("ask").value() == UnsafeAction::PROMPT);
    REQUIRE(unsafe_action_from_string("fail").has_value());
    CHECK(unsafe_action_from_string("fail").value() == UnsafeAction::ABORT);
    CHECK_FALSE(unsafe_action_from_string("pause").has_value());

    REQUIRE(osv_refresh_mode_from_string("PeRiOdIc").has_value());
    CHECK(osv_refresh_mode_from_string("PeRiOdIc").value() == OsvRefreshMode::PERIODIC);
    CHECK_FALSE(osv_refresh_mode_from_string("hourly").has_value());

    REQUIRE(sbom_output_format_from_string("cyclonedx-json").has_value());
    CHECK(sbom_output_format_from_string("cyclonedx-json").value() == SbomOutputFormat::CYCLONEDX_JSON);
    CHECK_FALSE(sbom_output_format_from_string("xml").has_value());
}

TEST_CASE("configuration resolves registry paths for directories and files", "[unit][configuration][path]") {
    const std::filesystem::path home = reqpack_user_home();

    CHECK(registry_database_directory("/tmp/reqpack/registry.lua") == std::filesystem::path("/tmp/reqpack"));
    CHECK(registry_source_file_path("/tmp/reqpack/registry.lua") == std::filesystem::path("/tmp/reqpack/registry.lua"));
    CHECK(registry_database_directory("/tmp/reqpack/registry") == std::filesystem::path("/tmp/reqpack/registry"));
    CHECK(registry_source_file_path("/tmp/reqpack/registry") == std::filesystem::path("/tmp/reqpack/registry/registry.lua"));

    CHECK(registry_database_directory("~/registry") == home / "registry");
    CHECK(registry_source_file_path("~/registry") == home / "registry" / "registry.lua");
}

TEST_CASE("configuration loads and normalizes registry source entries from lua", "[unit][configuration][parse]") {
    TempDir tempDir{"reqpack-config-registry-sources"};
    const std::filesystem::path sourcePath = tempDir.path() / "sources.lua";
    const std::filesystem::path home = reqpack_user_home();

    write_file(sourcePath, R"(
        return {
            sources = {
                DNF = {
                    source = "~/plugins/dnf.lua",
                    description = "DNF source",
                },
                Maven = {
                    alias = true,
                    target = "CENTRAL",
                    description = "Alias target",
                },
                Yum = "https://example.test/yum.lua",
                Empty = {
                    description = "missing source",
                },
                [1] = "ignored",
            },
        }
    )");

    const RegistrySourceMap sources = load_registry_sources_from_lua(sourcePath);
    REQUIRE(sources.size() == 3);
    REQUIRE(sources.contains("dnf"));
    REQUIRE(sources.contains("maven"));
    REQUIRE(sources.contains("yum"));

    CHECK(std::filesystem::path(sources.at("dnf").source) == home / "plugins/dnf.lua");
    CHECK_FALSE(sources.at("dnf").alias);
    CHECK(sources.at("dnf").description == "DNF source");

    CHECK(sources.at("maven").alias);
    CHECK(sources.at("maven").source == "central");
    CHECK(sources.at("maven").description == "Alias target");

    CHECK(sources.at("yum").source == "https://example.test/yum.lua");
}

TEST_CASE("configuration merges downloader, registry, database, and overlay sources in order", "[unit][configuration][merge]") {
    TempDir tempDir{"reqpack-config-registry-merge"};
    const std::filesystem::path registryDir = tempDir.path() / "registry-db";
    const std::filesystem::path overlayPath = tempDir.path() / "overlay.lua";

    write_file(registryDir / "registry.lua", R"(
        return {
            sources = {
                DNF = "https://registry.test/dnf.lua",
                Maven = {
                    alias = true,
                    source = "CENTRAL",
                },
            },
        }
    )");
    write_file(overlayPath, R"(
        return {
            sources = {
                DNF = "https://overlay.test/dnf.lua",
                Brew = "https://overlay.test/brew.lua",
            },
        }
    )");

    ReqPackConfig config;
    config.downloader.pluginSources["dnf"] = "https://downloader.test/dnf.lua";
    config.registry.sources["dnf"] = RegistrySourceEntry{.source = "https://config.test/dnf.lua"};
    config.registry.sources["apt"] = RegistrySourceEntry{.source = "https://config.test/apt.lua"};
    config.registry.databasePath = registryDir.string();
    config.registry.overlayPath = overlayPath.string();

    const RegistrySourceMap sources = collect_registry_sources(config);
    REQUIRE(sources.contains("dnf"));
    REQUIRE(sources.contains("apt"));
    REQUIRE(sources.contains("maven"));
    REQUIRE(sources.contains("brew"));

    CHECK(sources.at("dnf").source == "https://overlay.test/dnf.lua");
    CHECK(sources.at("apt").source == "https://config.test/apt.lua");
    CHECK(sources.at("maven").alias);
    CHECK(sources.at("maven").source == "central");
    CHECK(sources.at("brew").source == "https://overlay.test/brew.lua");
}

TEST_CASE("configuration loads lua config, expands paths, and preserves fallback on invalid fields", "[unit][configuration][load]") {
    TempDir tempDir{"reqpack-config-load"};
    const std::filesystem::path configPath = tempDir.path() / "config.lua";
    const std::filesystem::path home = reqpack_user_home();

    write_file(configPath, R"(
        return {
            logging = {
                level = "warning",
                filePath = "~/logs/reqpack.log",
            },
            security = {
                severityThreshold = "not-real",
                onUnsafe = "ask",
                osvDatabasePath = "~/osv-db",
                osvOverlayPath = "~/security-overlay.lua",
                osvRefreshMode = "periodic",
                osvRefreshIntervalSeconds = 900,
                strictEcosystemMapping = true,
                includeWithdrawnInReport = true,
                ignoreVulnerabilityIds = { "CVE-1", "GHSA-2" },
                allowVulnerabilityIds = { "CVE-3" },
                osvEcosystemMap = {
                    DNF = "Debian",
                },
            },
            reports = {
                enabled = true,
                format = "json",
                outputPath = "~/reports/reqpack.json",
            },
            planner = {
                systemAliases = {
                    Brew = "APT",
                },
            },
            downloader = {
                pluginSources = {
                    Maven = "https://example.test/maven.lua",
                },
            },
            registry = {
                databasePath = "~/registry-db",
                pluginDirectory = "~/plugins",
                sources = {
                    DNF = "https://example.test/dnf.lua",
                },
            },
            interaction = {
                interactive = false,
            },
            sbom = {
                defaultFormat = "cyclonedx-json",
                defaultOutputPath = "~/sbom-out.json",
                prettyPrint = false,
                includeDependencyEdges = false,
            },
            rqp = {
                repositories = {
                    "https://packages.example.test/rqp/index.json",
                    "file:///srv/rqp/index.json",
                },
                statePath = "~/rqp-state",
            },
        }
    )");

    ReqPackConfig fallback;
    fallback.security.severityThreshold = SeverityLevel::HIGH;

    const ReqPackConfig config = load_config_from_lua(configPath, fallback);
    CHECK(config.logging.level == LogLevel::WARN);
    CHECK(std::filesystem::path(config.logging.filePath) == home / "logs/reqpack.log");
    CHECK(config.security.severityThreshold == SeverityLevel::HIGH);
    CHECK(config.security.onUnsafe == UnsafeAction::PROMPT);
    CHECK(std::filesystem::path(config.security.osvDatabasePath) == home / "osv-db");
    CHECK(std::filesystem::path(config.security.osvOverlayPath) == home / "security-overlay.lua");
    CHECK(config.security.osvRefreshMode == OsvRefreshMode::PERIODIC);
    CHECK(config.security.osvRefreshIntervalSeconds == 900);
    CHECK(config.security.strictEcosystemMapping);
    CHECK(config.security.includeWithdrawnInReport);
    CHECK(config.security.ignoreVulnerabilityIds == std::vector<std::string>{"CVE-1", "GHSA-2"});
    CHECK(config.security.allowVulnerabilityIds == std::vector<std::string>{"CVE-3"});
    REQUIRE(config.security.osvEcosystemMap.contains("dnf"));
    CHECK(config.security.osvEcosystemMap.at("dnf") == "Debian");
    CHECK(config.reports.enabled);
    CHECK(config.reports.format == ReportFormat::JSON);
    CHECK(std::filesystem::path(config.reports.outputPath) == home / "reports/reqpack.json");
    REQUIRE(config.planner.systemAliases.contains("brew"));
    CHECK(config.planner.systemAliases.at("brew") == "apt");
    REQUIRE(config.downloader.pluginSources.contains("maven"));
    CHECK(config.downloader.pluginSources.at("maven") == "https://example.test/maven.lua");
    CHECK(std::filesystem::path(config.registry.databasePath) == home / "registry-db");
    CHECK(std::filesystem::path(config.registry.pluginDirectory) == home / "plugins");
    REQUIRE(config.registry.sources.contains("dnf"));
    CHECK(config.registry.sources.at("dnf").source == "https://example.test/dnf.lua");
    CHECK_FALSE(config.interaction.interactive);
    CHECK(config.sbom.defaultFormat == SbomOutputFormat::CYCLONEDX_JSON);
    CHECK(std::filesystem::path(config.sbom.defaultOutputPath) == home / "sbom-out.json");
    CHECK_FALSE(config.sbom.prettyPrint);
    CHECK_FALSE(config.sbom.includeDependencyEdges);
    CHECK(config.rqp.repositories == std::vector<std::string>{
        "https://packages.example.test/rqp/index.json",
        "file:///srv/rqp/index.json",
    });
    CHECK(std::filesystem::path(config.rqp.statePath) == home / "rqp-state");
}

TEST_CASE("configuration falls back for missing or invalid lua config files", "[unit][configuration][load]") {
    TempDir tempDir{"reqpack-config-fallback"};
    ReqPackConfig fallback;
    fallback.applicationName = "FallbackApp";
    fallback.interaction.interactive = true;

    const ReqPackConfig missing = load_config_from_lua(tempDir.path() / "missing.lua", fallback);
    CHECK(missing.applicationName == "FallbackApp");
    CHECK(missing.interaction.interactive);

    const std::filesystem::path invalidPath = tempDir.path() / "invalid.lua";
    write_file(invalidPath, "this is not valid lua");
    const ReqPackConfig invalid = load_config_from_lua(invalidPath, fallback);
    CHECK(invalid.applicationName == "FallbackApp");
    CHECK(invalid.interaction.interactive);

    const std::filesystem::path globalConfigPath = tempDir.path() / "global-config.lua";
    write_file(globalConfigPath, R"(
        config = {
            interaction = {
                interactive = false,
            },
        }
    )");
    const ReqPackConfig globalConfig = load_config_from_lua(globalConfigPath, fallback);
    CHECK_FALSE(globalConfig.interaction.interactive);
}

TEST_CASE("remote user loader parses users and defaults admin flag", "[unit][configuration][remote]") {
    TempDir tempDir{"reqpack-remote-users"};
    const std::filesystem::path remotePath = tempDir.path() / "remote.lua";

    write_file(remotePath, R"(
        return {
            users = {
                alice = {
                    token = "user-token",
                },
                root = {
                    username = "root",
                    password = "admin-pass",
                    isAdmin = true,
                },
                broken = {
                    isAdmin = true,
                },
            },
        }
    )");

    const std::vector<RemoteUser> users = load_remote_users(remotePath);
    REQUIRE(users.size() == 2);

    const auto alice = std::find_if(users.begin(), users.end(), [](const RemoteUser& user) {
        return user.id == "alice";
    });
    REQUIRE(alice != users.end());
    REQUIRE(alice->token.has_value());
    CHECK(alice->token.value() == "user-token");
    CHECK_FALSE(alice->isAdmin);

    const auto root = std::find_if(users.begin(), users.end(), [](const RemoteUser& user) {
        return user.id == "root";
    });
    REQUIRE(root != users.end());
    REQUIRE(root->username.has_value());
    CHECK(root->username.value() == "root");
    REQUIRE(root->password.has_value());
    CHECK(root->password.value() == "admin-pass");
    CHECK(root->isAdmin);
}
