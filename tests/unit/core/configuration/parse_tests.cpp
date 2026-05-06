#include <catch2/catch.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
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

class ScopedEnvVar {
public:
    ScopedEnvVar(std::string name, std::string value)
        : name_(std::move(name)) {
        if (const char* existing = std::getenv(name_.c_str())) {
            previous_ = std::string(existing);
        }
        ::setenv(name_.c_str(), value.c_str(), 1);
    }

    ~ScopedEnvVar() {
        if (previous_.has_value()) {
            ::setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    REQUIRE(output.is_open());
    output << content;
    output.close();
}

std::filesystem::path reqpack_user_home() {
    const char* home = std::getenv("HOME");
    return home != nullptr ? std::filesystem::path(home) : std::filesystem::path{};
}

TEST_CASE("configuration resolves XDG directories with standard fallbacks", "[unit][configuration][xdg]") {
    TempDir tempDir{"reqpack-xdg-roots"};
    ScopedEnvVar home{"HOME", (tempDir.path() / "home").string()};
    ScopedEnvVar configHome{"XDG_CONFIG_HOME", ""};
    ScopedEnvVar dataHome{"XDG_DATA_HOME", ""};
    ScopedEnvVar cacheHome{"XDG_CACHE_HOME", ""};

    CHECK(reqpack_config_directory() == tempDir.path() / "home" / ".config" / "reqpack");
    CHECK(reqpack_data_directory() == tempDir.path() / "home" / ".local" / "share" / "reqpack");
    CHECK(reqpack_cache_directory() == tempDir.path() / "home" / ".cache" / "reqpack");
    CHECK(default_reqpack_config_path() == tempDir.path() / "home" / ".config" / "reqpack" / "config.lua");
    CHECK(default_reqpack_registry_path() == tempDir.path() / "home" / ".local" / "share" / "reqpack" / "registry");
    CHECK(default_reqpack_plugin_directory() == tempDir.path() / "home" / ".local" / "share" / "reqpack" / "plugins");
    CHECK(default_reqpack_repo_cache_path() == tempDir.path() / "home" / ".local" / "share" / "reqpack" / "repos");
    CHECK(default_reqpack_history_path() == tempDir.path() / "home" / ".local" / "share" / "reqpack" / "history");
    CHECK(default_reqpack_rqp_state_path() == tempDir.path() / "home" / ".local" / "share" / "reqpack" / "rqp" / "state");
    CHECK(default_reqpack_self_update_repo_path() == tempDir.path() / "home" / ".local" / "share" / "reqpack" / "self" / "repo");
    CHECK(default_reqpack_self_update_build_path() == tempDir.path() / "home" / ".local" / "share" / "reqpack" / "self" / "build");
    CHECK(default_reqpack_self_update_binary_directory() == tempDir.path() / "home" / ".local" / "share" / "reqpack" / "self" / "bin");
    CHECK(default_reqpack_self_update_link_path() == tempDir.path() / "home" / ".local" / "bin" / "rqp");
    CHECK(default_reqpack_security_index_path() == tempDir.path() / "home" / ".local" / "share" / "reqpack" / "security" / "index");
    CHECK(default_reqpack_osv_database_path() == tempDir.path() / "home" / ".local" / "share" / "reqpack" / "security" / "osv");
    CHECK(default_reqpack_transaction_path() == tempDir.path() / "home" / ".cache" / "reqpack" / "transactions");
    CHECK(default_reqpack_security_cache_path() == tempDir.path() / "home" / ".cache" / "reqpack" / "security" / "cache");
    CHECK(default_remote_profiles_path() == tempDir.path() / "home" / ".config" / "reqpack" / "remote.lua");
}

TEST_CASE("configuration honors explicit XDG directories", "[unit][configuration][xdg]") {
    TempDir tempDir{"reqpack-xdg-explicit"};
    ScopedEnvVar configHome{"XDG_CONFIG_HOME", (tempDir.path() / "cfg").string()};
    ScopedEnvVar dataHome{"XDG_DATA_HOME", (tempDir.path() / "data").string()};
    ScopedEnvVar cacheHome{"XDG_CACHE_HOME", (tempDir.path() / "cache").string()};

    const ReqPackConfig config;
    CHECK(reqpack_config_directory() == tempDir.path() / "cfg" / "reqpack");
    CHECK(reqpack_data_directory() == tempDir.path() / "data" / "reqpack");
    CHECK(reqpack_cache_directory() == tempDir.path() / "cache" / "reqpack");
    CHECK(std::filesystem::path(config.registry.databasePath) == tempDir.path() / "data" / "reqpack" / "registry");
    CHECK(std::filesystem::path(config.registry.pluginDirectory) == tempDir.path() / "data" / "reqpack" / "plugins");
    CHECK(config.registry.remoteUrl.empty());
    CHECK(config.registry.remoteBranch == "main");
    CHECK(config.registry.remotePluginsPath == "registry");
    CHECK(std::filesystem::path(config.history.historyPath) == tempDir.path() / "data" / "reqpack" / "history");
    CHECK(std::filesystem::path(config.rqp.statePath) == tempDir.path() / "data" / "reqpack" / "rqp" / "state");
    CHECK(std::filesystem::path(config.selfUpdate.repoPath) == tempDir.path() / "data" / "reqpack" / "self" / "repo");
    CHECK(std::filesystem::path(config.selfUpdate.buildPath) == tempDir.path() / "data" / "reqpack" / "self" / "build");
    CHECK(std::filesystem::path(config.selfUpdate.binaryDirectory) == tempDir.path() / "data" / "reqpack" / "self" / "bin");
    CHECK(std::filesystem::path(config.selfUpdate.linkPath) == reqpack_user_home() / ".local" / "bin" / "rqp");
    CHECK(std::filesystem::path(config.security.indexPath) == tempDir.path() / "data" / "reqpack" / "security" / "index");
    CHECK(std::filesystem::path(config.security.osvDatabasePath) == tempDir.path() / "data" / "reqpack" / "security" / "osv");
    CHECK(std::filesystem::path(config.execution.transactionDatabasePath) == tempDir.path() / "cache" / "reqpack" / "transactions");
    CHECK(std::filesystem::path(config.security.cachePath) == tempDir.path() / "cache" / "reqpack" / "security" / "cache");
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

    REQUIRE(audit_output_format_from_string("cyclonedx-vex-json").has_value());
    CHECK(audit_output_format_from_string("cyclonedx-vex-json").value() == AuditOutputFormat::CYCLONEDX_VEX_JSON);
    REQUIRE(audit_output_format_from_string("sarif").has_value());
    CHECK(audit_output_format_from_string("sarif").value() == AuditOutputFormat::SARIF);
    CHECK_FALSE(audit_output_format_from_string("xml").has_value());

    REQUIRE(execution_jobs_mode_from_string("FiXeD").has_value());
    CHECK(execution_jobs_mode_from_string("FiXeD").value() == ExecutionJobsMode::FIXED);
    REQUIRE(execution_jobs_mode_from_string("max").has_value());
    CHECK(execution_jobs_mode_from_string("max").value() == ExecutionJobsMode::MAX);
    CHECK_FALSE(execution_jobs_mode_from_string("auto").has_value());
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
                    ecosystemScopes = { "demo-osv", "Rubygems" },
                    writeScopes = {
                        { kind = "Temp" },
                        { kind = "user-home-subpath", value = ".cache/demo" },
                    },
                    networkScopes = {
                        { host = "API.OSV.DEV", scheme = "HTTPS", pathPrefix = "/v1" },
                    },
                    scriptSha256 = string.rep("A", 64),
                    bootstrapSha256 = string.rep("B", 64),
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
    CHECK(sources.at("dnf").ecosystemScopes == std::vector<std::string>{"demo-osv", "rubygems"});
    REQUIRE(sources.at("dnf").writeScopes.size() == 2);
    CHECK(sources.at("dnf").writeScopes[0].kind == "temp");
    CHECK(sources.at("dnf").writeScopes[0].value.empty());
    CHECK(sources.at("dnf").writeScopes[1].kind == "user-home-subpath");
    CHECK(sources.at("dnf").writeScopes[1].value == ".cache/demo");
    REQUIRE(sources.at("dnf").networkScopes.size() == 1);
    CHECK(sources.at("dnf").networkScopes[0].host == "api.osv.dev");
    CHECK(sources.at("dnf").networkScopes[0].scheme == "https");
    CHECK(sources.at("dnf").networkScopes[0].pathPrefix == "/v1");
    CHECK(sources.at("dnf").scriptSha256 == std::string(64, 'a'));
    CHECK(sources.at("dnf").bootstrapSha256 == std::string(64, 'b'));

    CHECK(sources.at("maven").alias);
    CHECK(sources.at("maven").source == "central");
    CHECK(sources.at("maven").description == "Alias target");

    CHECK(sources.at("yum").source == "https://example.test/yum.lua");
}

TEST_CASE("configuration loads structured logging settings from lua", "[unit][configuration][parse]") {
    TempDir tempDir{"reqpack-config-logging"};
    const std::filesystem::path configPath = tempDir.path() / "config.lua";

    write_file(configPath, R"(
        return {
            logging = {
                level = "debug",
                consoleOutput = true,
                fileOutput = true,
                filePath = "~/reqpack.log",
                structuredFileOutput = true,
                structuredFilePath = "~/reqpack.jsonl",
                captureDisplayEvents = false,
                enabledCategories = { "Network", "plugin", "network" },
            },
        }
    )");

    const ReqPackConfig config = load_config_from_lua(configPath, default_reqpack_config());
    const std::filesystem::path home = reqpack_user_home();

    CHECK(config.logging.level == LogLevel::DEBUG);
    CHECK(config.logging.consoleOutput);
    CHECK(config.logging.fileOutput);
    CHECK(std::filesystem::path(config.logging.filePath) == home / "reqpack.log");
    CHECK(config.logging.structuredFileOutput);
    CHECK(std::filesystem::path(config.logging.structuredFilePath) == home / "reqpack.jsonl");
    CHECK_FALSE(config.logging.captureDisplayEvents);
    CHECK(config.logging.enabledCategories == std::vector<std::string>({"network", "plugin"}));
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
    ScopedEnvVar archivePassword{"REQPACK_TEST_ARCHIVE_PASSWORD", "archive-secret"};

    write_file(configPath, R"(
        return {
            logging = {
                level = "warning",
                filePath = "~/logs/reqpack.log",
            },
            security = {
                autoFetch = true,
                requireThinLayer = true,
                defaultGateway = "security",
                cachePath = "~/security-cache",
                indexPath = "~/security-index",
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
                ecosystemMap = {
                    Maven = "Maven",
                },
                gateways = {
                    security = {
                        backends = { "osv", "snyk" },
                    },
                },
                backends = {
                    osv = {
                        feedUrl = "https://mirror.example.test/osv",
                        refreshMode = "always",
                        refreshIntervalSeconds = 123,
                        overlayPath = "~/backend-overlay.json",
                    },
                },
            },
            reports = {
                enabled = true,
                format = "json",
                outputPath = "~/reports/reqpack.json",
            },
            execution = {
                jobs = 8,
                jobsMode = "max",
            },
            planner = {
                systemAliases = {
                    Brew = "APT",
                },
                proxies = {
                    Java = {
                        default = "Maven",
                        targets = { "Maven", "Gradle" },
                        options = {
                            strategy = "default-first",
                        },
                    },
                },
            },
            downloader = {
                pluginSources = {
                    Maven = "https://example.test/maven.lua",
                },
            },
            registry = {
                databasePath = "~/registry-db",
                remoteUrl = "https://github.com/example/reqpack-plugin-registry.git",
                remoteBranch = "stable",
                remotePluginsPath = "main-plugins",
                pluginDirectory = "~/plugins",
                sources = {
                    DNF = "https://example.test/dnf.lua",
                },
            },
            interaction = {
                interactive = false,
            },
            archives = {
                password = "$REQPACK_TEST_ARCHIVE_PASSWORD",
            },
            sbom = {
                defaultFormat = "cyclonedx-json",
                defaultOutputPath = "~/sbom-out.json",
                prettyPrint = false,
                includeDependencyEdges = false,
                skipMissingPackages = true,
            },
            rqp = {
                repositories = {
                    "https://packages.example.test/rqp/index.json",
                    "file:///srv/rqp/index.json",
                },
                statePath = "~/rqp-state",
            },
            selfUpdate = {
                repoUrl = "https://example.test/ReqPack.git",
                branch = "stable",
                repoPath = "~/self/repo",
                buildPath = "~/self/build",
                binaryDirectory = "~/self/bin",
                linkPath = "~/bin/rqp",
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
    CHECK(config.security.autoFetch);
    CHECK(config.security.requireThinLayer);
    CHECK(config.security.defaultGateway == "security");
    CHECK(std::filesystem::path(config.security.cachePath) == home / "security-cache");
    CHECK(std::filesystem::path(config.security.indexPath) == home / "security-index");
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
    REQUIRE(config.security.ecosystemMap.contains("maven"));
    CHECK(config.security.ecosystemMap.at("maven") == "Maven");
    REQUIRE(config.security.gateways.contains("security"));
    CHECK(config.security.gateways.at("security").backends == std::vector<std::string>{"osv", "snyk"});
    REQUIRE(config.security.backends.contains("osv"));
    CHECK(config.security.backends.at("osv").feedUrl == "https://mirror.example.test/osv");
    CHECK(config.security.backends.at("osv").refreshMode == OsvRefreshMode::ALWAYS);
    CHECK(config.security.backends.at("osv").refreshIntervalSeconds == 123);
    CHECK(std::filesystem::path(config.security.backends.at("osv").overlayPath) == home / "backend-overlay.json");
    CHECK(config.reports.enabled);
    CHECK(config.reports.format == ReportFormat::JSON);
    CHECK(std::filesystem::path(config.reports.outputPath) == home / "reports/reqpack.json");
    CHECK(config.execution.jobs == 8);
    CHECK(config.execution.jobsMode == ExecutionJobsMode::MAX);
    REQUIRE(config.planner.systemAliases.contains("brew"));
    CHECK(config.planner.systemAliases.at("brew") == "apt");
    REQUIRE(config.planner.proxies.contains("java"));
    CHECK(config.planner.proxies.at("java").defaultTarget == "maven");
    CHECK(config.planner.proxies.at("java").targets == std::vector<std::string>{"maven", "gradle"});
    REQUIRE(config.planner.proxies.at("java").options.contains("strategy"));
    CHECK(config.planner.proxies.at("java").options.at("strategy") == "default-first");
    REQUIRE(config.downloader.pluginSources.contains("maven"));
    CHECK(config.downloader.pluginSources.at("maven") == "https://example.test/maven.lua");
    CHECK(std::filesystem::path(config.registry.databasePath) == home / "registry-db");
    CHECK(config.registry.remoteUrl == "https://github.com/example/reqpack-plugin-registry.git");
    CHECK(config.registry.remoteBranch == "stable");
    CHECK(config.registry.remotePluginsPath == "main-plugins");
    CHECK(std::filesystem::path(config.registry.pluginDirectory) == home / "plugins");
    REQUIRE(config.registry.sources.contains("dnf"));
    CHECK(config.registry.sources.at("dnf").source == "https://example.test/dnf.lua");
    CHECK_FALSE(config.interaction.interactive);
    CHECK(config.archives.password == "archive-secret");
    CHECK(config.sbom.defaultFormat == SbomOutputFormat::CYCLONEDX_JSON);
    CHECK(std::filesystem::path(config.sbom.defaultOutputPath) == home / "sbom-out.json");
    CHECK_FALSE(config.sbom.prettyPrint);
    CHECK_FALSE(config.sbom.includeDependencyEdges);
    CHECK(config.sbom.skipMissingPackages);
    CHECK(config.rqp.repositories == std::vector<std::string>{
        "https://packages.example.test/rqp/index.json",
        "file:///srv/rqp/index.json",
    });
    CHECK(std::filesystem::path(config.rqp.statePath) == home / "rqp-state");
    CHECK(config.selfUpdate.repoUrl == "https://example.test/ReqPack.git");
    CHECK(config.selfUpdate.branch == "stable");
    CHECK(std::filesystem::path(config.selfUpdate.repoPath) == home / "self/repo");
    CHECK(std::filesystem::path(config.selfUpdate.buildPath) == home / "self/build");
    CHECK(std::filesystem::path(config.selfUpdate.binaryDirectory) == home / "self/bin");
    CHECK(std::filesystem::path(config.selfUpdate.linkPath) == home / "bin/rqp");
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

TEST_CASE("archive password resolution prefers config then environment fallback", "[unit][configuration][load]") {
    ReqPackConfig config;
    ScopedEnvVar password{"REQPACK_ARCHIVE_PASSWORD", "from-env"};

    config.archives.password = "from-config";
    CHECK(resolve_archive_password(config) == "from-config");

    config.archives.password.clear();
    CHECK(resolve_archive_password(config) == "from-env");
}

TEST_CASE("configuration resolves execution jobs from fixed and max modes", "[unit][configuration][execution]") {
    ReqPackConfig fixed;
    fixed.execution.jobs = 5;
    fixed.execution.jobsMode = ExecutionJobsMode::FIXED;
    CHECK(resolved_execution_jobs(fixed) == 5);

    ReqPackConfig maxConfig;
    maxConfig.execution.jobs = 1;
    maxConfig.execution.jobsMode = ExecutionJobsMode::MAX;
    CHECK(resolved_execution_jobs(maxConfig) >= 1);
}

TEST_CASE("configuration parses structured repositories and preserves flat extras", "[unit][configuration][load]") {
    TempDir tempDir{"reqpack-config-repositories"};
    const std::filesystem::path configPath = tempDir.path() / "config.lua";
    const std::filesystem::path home = reqpack_user_home();
    ScopedEnvVar token{"REQPACK_TEST_REPO_TOKEN", "secret-token"};

    write_file(configPath, R"(
        return {
            repositories = {
                Maven = {
                    {
                        id = "corp",
                        url = "https://repo.example.test/maven-public",
                        priority = 5,
                        enabled = false,
                        type = "default",
                        auth = {
                            type = "token",
                            token = "$REQPACK_TEST_REPO_TOKEN",
                            headerName = "X-Repo-Token",
                        },
                        validation = {
                            checksum = "fail",
                            tlsVerify = false,
                        },
                        scope = {
                            include = { "com.mycompany.*" },
                            exclude = { "com.mycompany.legacy.*" },
                        },
                        snapshots = true,
                        layout = "default",
                        score = 2.5,
                        tags = { "internal", "fast" },
                        metadata = {
                            nested = "ignored",
                        },
                    },
                    {
                        id = "ssh-repo",
                        url = "ssh://git@example.test/repo",
                        auth = {
                            type = "ssh",
                            sshKey = "~/.ssh/repo.key",
                        },
                    },
                    {
                        id = "corp",
                        url = "https://duplicate.example.test/repo",
                    },
                    {
                        id = "invalid-auth",
                        url = "https://invalid-auth.example.test/repo",
                        auth = {
                            type = "token",
                        },
                    },
                    {
                        id = "warn-default",
                        url = "https://warn.example.test/repo",
                        validation = {
                            checksum = "broken",
                        },
                        scope = {
                            include = "not-a-list",
                            exclude = { 1, 2 },
                        },
                    },
                },
            },
        }
    )");

    const ReqPackConfig config = load_config_from_lua(configPath);
    REQUIRE(config.repositories.contains("maven"));
    CHECK_FALSE(config.repositories.contains("Maven"));

    const std::vector<RepositoryEntry>& repositories = config.repositories.at("maven");
    REQUIRE(repositories.size() == 3);

    const RepositoryEntry& corp = repositories[0];
    CHECK(corp.id == "corp");
    CHECK(corp.url == "https://repo.example.test/maven-public");
    CHECK(corp.priority == 5);
    CHECK_FALSE(corp.enabled);
    CHECK(corp.type == "default");
    CHECK(corp.auth.type == RepositoryAuthType::TOKEN);
    CHECK(corp.auth.token == "secret-token");
    CHECK(corp.auth.headerName == "X-Repo-Token");
    CHECK(corp.validation.checksum == RepositoryChecksumPolicy::FAIL);
    CHECK_FALSE(corp.validation.tlsVerify);
    CHECK(corp.scope.include == std::vector<std::string>{"com.mycompany.*"});
    CHECK(corp.scope.exclude == std::vector<std::string>{"com.mycompany.legacy.*"});
    REQUIRE(corp.extras.contains("snapshots"));
    CHECK(std::get<bool>(corp.extras.at("snapshots")));
    REQUIRE(corp.extras.contains("layout"));
    CHECK(std::get<std::string>(corp.extras.at("layout")) == "default");
    REQUIRE(corp.extras.contains("score"));
    CHECK(std::get<double>(corp.extras.at("score")) == Approx(2.5));
    REQUIRE(corp.extras.contains("tags"));
    CHECK(std::get<std::vector<std::string>>(corp.extras.at("tags")) == std::vector<std::string>{"internal", "fast"});
    CHECK_FALSE(corp.extras.contains("metadata"));

    const RepositoryEntry& sshRepo = repositories[1];
    CHECK(sshRepo.id == "ssh-repo");
    CHECK(sshRepo.priority == 100);
    CHECK(sshRepo.enabled);
    CHECK(sshRepo.auth.type == RepositoryAuthType::SSH);
    CHECK(std::filesystem::path(sshRepo.auth.sshKey) == home / ".ssh" / "repo.key");
    CHECK(sshRepo.validation.checksum == RepositoryChecksumPolicy::WARN);
    CHECK(sshRepo.validation.tlsVerify);

    const RepositoryEntry& warnDefault = repositories[2];
    CHECK(warnDefault.id == "warn-default");
    CHECK(warnDefault.validation.checksum == RepositoryChecksumPolicy::WARN);
    CHECK(warnDefault.scope.include.empty());
    CHECK(warnDefault.scope.exclude.empty());

    const std::vector<RepositoryEntry> ordered = repositories_for_ecosystem(config, "MAVEN");
    REQUIRE(ordered.size() == 3);
    CHECK(ordered[0].id == "corp");
    CHECK(ordered[1].id == "ssh-repo");
    CHECK(ordered[2].id == "warn-default");
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
