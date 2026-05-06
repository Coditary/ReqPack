#include <catch2/catch.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "cli/cli.h"
#include "core/configuration.h"
#include "test_helpers.h"

namespace {

std::filesystem::path reqpack_user_home() {
    const char* home = std::getenv("HOME");
    return home != nullptr ? std::filesystem::path(home) : std::filesystem::path{};
}

}  // namespace

TEST_CASE("configuration applies CLI overrides and expands path fields", "[unit][configuration][cli]") {
    ReqPackConfig base;
    ReqPackConfigOverrides overrides;
    overrides.logLevel = LogLevel::DEBUG;
    overrides.logPattern = "[%l] %v";
    overrides.logFilePath = "~/logs/reqpack.log";
    overrides.fileOutput = true;
    overrides.structuredFileOutput = true;
    overrides.structuredLogFilePath = "~/logs/reqpack.jsonl";
    overrides.captureDisplayEvents = false;
    overrides.enabledLogCategories = {"network", "plugin", "network"};
    overrides.enableBacktrace = true;
    overrides.backtraceSize = 64;
    overrides.severityThreshold = SeverityLevel::MEDIUM;
    overrides.osvDatabasePath = "~/osv-db";
    overrides.osvRefreshMode = OsvRefreshMode::ALWAYS;
    overrides.osvRefreshIntervalSeconds = 60;
    overrides.osvOverlayPath = "~/overlay.json";
    overrides.ignoreVulnerabilityIds = {"CVE-1"};
    overrides.allowVulnerabilityIds = {"CVE-2"};
    overrides.onUnresolvedVersion = UnsafeAction::PROMPT;
    overrides.strictEcosystemMapping = true;
    overrides.includeWithdrawnInReport = true;
    overrides.reportEnabled = true;
    overrides.reportFormat = ReportFormat::CYCLONEDX;
    overrides.reportOutputPath = "~/reports/bom.json";
    overrides.dryRun = true;
    overrides.jobs = 4;
    overrides.jobsMode = ExecutionJobsMode::FIXED;
    overrides.enableProxyExpansion = false;
    overrides.proxyDefaultTargets["java"] = "gradle";
    overrides.registryPath = "~/registry";
    overrides.pluginDirectory = "~/plugins";
    overrides.autoLoadPlugins = false;
    overrides.interactive = false;
    overrides.archivePassword = "secret-archive-password";
    overrides.sbomDefaultFormat = SbomOutputFormat::JSON;
    overrides.sbomDefaultOutputPath = "~/exports/sbom.json";
    overrides.sbomPrettyPrint = false;
    overrides.sbomIncludeDependencyEdges = false;
    overrides.sbomSkipMissingPackages = true;

    const ReqPackConfig config = apply_config_overrides(base, overrides);
    const std::filesystem::path home = reqpack_user_home();

    CHECK(config.logging.level == LogLevel::DEBUG);
    CHECK(config.logging.pattern == "[%l] %v");
    CHECK(config.logging.fileOutput);
    CHECK(std::filesystem::path(config.logging.filePath) == home / "logs/reqpack.log");
    CHECK(config.logging.structuredFileOutput);
    CHECK(std::filesystem::path(config.logging.structuredFilePath) == home / "logs/reqpack.jsonl");
    CHECK_FALSE(config.logging.captureDisplayEvents);
    CHECK(config.logging.enabledCategories == std::vector<std::string>({"network", "plugin"}));
    CHECK(config.logging.enableBacktrace);
    CHECK(config.logging.backtraceSize == 64);
    CHECK(config.security.severityThreshold == SeverityLevel::MEDIUM);
    CHECK(std::filesystem::path(config.security.osvDatabasePath) == home / "osv-db");
    CHECK(config.security.osvRefreshMode == OsvRefreshMode::ALWAYS);
    CHECK(config.security.osvRefreshIntervalSeconds == 60);
    CHECK(std::filesystem::path(config.security.osvOverlayPath) == home / "overlay.json");
    CHECK(config.security.ignoreVulnerabilityIds == std::vector<std::string>{"CVE-1"});
    CHECK(config.security.allowVulnerabilityIds == std::vector<std::string>{"CVE-2"});
    CHECK(config.security.onUnresolvedVersion == UnsafeAction::PROMPT);
    CHECK(config.security.strictEcosystemMapping);
    CHECK(config.security.includeWithdrawnInReport);
    CHECK(config.reports.enabled);
    CHECK(config.reports.format == ReportFormat::CYCLONEDX);
    CHECK(std::filesystem::path(config.reports.outputPath) == home / "reports/bom.json");
    CHECK(config.execution.dryRun);
    CHECK(config.execution.jobs == 4);
    CHECK(config.execution.jobsMode == ExecutionJobsMode::FIXED);
    CHECK_FALSE(config.planner.enableProxyExpansion);
    REQUIRE(config.planner.proxies.contains("java"));
    CHECK(config.planner.proxies.at("java").defaultTarget == "gradle");
    CHECK(std::filesystem::path(config.registry.databasePath) == home / "registry");
    CHECK(std::filesystem::path(config.registry.pluginDirectory) == home / "plugins");
    CHECK_FALSE(config.registry.autoLoadPlugins);
    CHECK_FALSE(config.interaction.interactive);
    CHECK(config.archives.password == "secret-archive-password");
    CHECK(config.sbom.defaultFormat == SbomOutputFormat::JSON);
    CHECK(std::filesystem::path(config.sbom.defaultOutputPath) == home / "exports/sbom.json");
    CHECK_FALSE(config.sbom.prettyPrint);
    CHECK_FALSE(config.sbom.includeDependencyEdges);
    CHECK(config.sbom.skipMissingPackages);
}

TEST_CASE("configuration consumes CLI flags with positional and inline values", "[unit][configuration][cli]") {
    SECTION("config flag with inline value") {
        const std::vector<std::string> arguments{"--config=custom.lua"};
        std::size_t index = 0;
        ReqPackConfigOverrides overrides;

        REQUIRE(consume_cli_config_flag(arguments, index, overrides));
        REQUIRE(overrides.configPath.has_value());
        CHECK(overrides.configPath.value() == std::filesystem::path("custom.lua"));
        CHECK(index == 0);
    }

    SECTION("log file consumes next argument and enables file output") {
        const std::vector<std::string> arguments{"--log-file", "~/reqpack.log"};
        std::size_t index = 0;
        ReqPackConfigOverrides overrides;

        REQUIRE(consume_cli_config_flag(arguments, index, overrides));
        REQUIRE(overrides.fileOutput.has_value());
        CHECK(overrides.fileOutput.value());
        REQUIRE(overrides.logFilePath.has_value());
        CHECK(overrides.logFilePath.value() == "~/reqpack.log");
        CHECK(index == 1);
    }

    SECTION("structured logging flags map to expected overrides") {
        {
            const std::vector<std::string> arguments{"--structured-log-file", "~/reqpack.jsonl"};
            std::size_t index = 0;
            ReqPackConfigOverrides overrides;

            REQUIRE(consume_cli_config_flag(arguments, index, overrides));
            REQUIRE(overrides.structuredFileOutput.has_value());
            CHECK(overrides.structuredFileOutput.value());
            REQUIRE(overrides.structuredLogFilePath.has_value());
            CHECK(overrides.structuredLogFilePath.value() == "~/reqpack.jsonl");
            CHECK(index == 1);
        }

        {
            const std::vector<std::string> arguments{"--log-category", "Network"};
            std::size_t index = 0;
            ReqPackConfigOverrides overrides;

            REQUIRE(consume_cli_config_flag(arguments, index, overrides));
            CHECK(overrides.enabledLogCategories == std::vector<std::string>{"network"});
            CHECK(index == 1);
        }

        {
            const std::vector<std::string> arguments{"--log-capture-display"};
            std::size_t index = 0;
            ReqPackConfigOverrides overrides;

            REQUIRE(consume_cli_config_flag(arguments, index, overrides));
            REQUIRE(overrides.captureDisplayEvents.has_value());
            CHECK(overrides.captureDisplayEvents.value());
        }

        {
            const std::vector<std::string> arguments{"--no-log-console"};
            std::size_t index = 0;
            ReqPackConfigOverrides overrides;

            REQUIRE(consume_cli_config_flag(arguments, index, overrides));
            REQUIRE(overrides.consoleOutput.has_value());
            CHECK_FALSE(overrides.consoleOutput.value());
        }
    }

    SECTION("prompt and registry flags map to expected overrides") {
        {
            const std::vector<std::string> arguments{"--prompt-on-unsafe"};
            std::size_t index = 0;
            ReqPackConfigOverrides overrides;

            REQUIRE(consume_cli_config_flag(arguments, index, overrides));
            REQUIRE(overrides.promptOnUnsafe.has_value());
            CHECK(overrides.promptOnUnsafe.value());
            REQUIRE(overrides.onUnsafe.has_value());
            CHECK(overrides.onUnsafe.value() == UnsafeAction::PROMPT);
        }

        {
            const std::vector<std::string> arguments{"--registry=/tmp/reqpack-registry"};
            std::size_t index = 0;
            ReqPackConfigOverrides overrides;

            REQUIRE(consume_cli_config_flag(arguments, index, overrides));
            REQUIRE(overrides.registryPath.has_value());
            CHECK(overrides.registryPath.value() == "/tmp/reqpack-registry");
        }

        {
            const std::vector<std::string> arguments{"--archive-password=secret"};
            std::size_t index = 0;
            ReqPackConfigOverrides overrides;

            REQUIRE(consume_cli_config_flag(arguments, index, overrides));
            REQUIRE(overrides.archivePassword.has_value());
            CHECK(overrides.archivePassword.value() == "secret");
        }
    }

    SECTION("jobs flags map to expected overrides") {
        {
            const std::vector<std::string> arguments{"--jobs", "3"};
            std::size_t index = 0;
            ReqPackConfigOverrides overrides;

            REQUIRE(consume_cli_config_flag(arguments, index, overrides));
            REQUIRE(overrides.jobs.has_value());
            CHECK(overrides.jobs.value() == 3);
            REQUIRE(overrides.jobsMode.has_value());
            CHECK(overrides.jobsMode.value() == ExecutionJobsMode::FIXED);
            CHECK(index == 1);
        }

        {
            const std::vector<std::string> arguments{"--jobs-max"};
            std::size_t index = 0;
            ReqPackConfigOverrides overrides;

            REQUIRE(consume_cli_config_flag(arguments, index, overrides));
            REQUIRE(overrides.jobsMode.has_value());
            CHECK(overrides.jobsMode.value() == ExecutionJobsMode::MAX);
            CHECK_FALSE(overrides.jobs.has_value());
        }

        {
            const std::vector<std::string> arguments{"--jobs", "0"};
            std::size_t index = 0;
            ReqPackConfigOverrides overrides;

            REQUIRE(consume_cli_config_flag(arguments, index, overrides));
            REQUIRE(overrides.errorMessage.has_value());
            CHECK(overrides.errorMessage.value() == "invalid value for --jobs: 0");
        }

        {
            const std::vector<std::string> arguments{"--jobs-max", "--jobs", "2"};
            std::size_t index = 0;
            ReqPackConfigOverrides overrides;

            REQUIRE(consume_cli_config_flag(arguments, index, overrides));
            CHECK_FALSE(overrides.errorMessage.has_value());
            index = 1;
            REQUIRE(consume_cli_config_flag(arguments, index, overrides));
            REQUIRE(overrides.errorMessage.has_value());
            CHECK(overrides.errorMessage.value() == "cannot combine --jobs with --jobs-max");
        }
    }

    SECTION("invalid score value is ignored and unknown flag is rejected") {
        const std::vector<std::string> arguments{"--score-threshold", "oops"};
        std::size_t index = 0;
        ReqPackConfigOverrides overrides;

        REQUIRE(consume_cli_config_flag(arguments, index, overrides));
        CHECK_FALSE(overrides.scoreThreshold.has_value());
        CHECK(index == 1);

        const std::vector<std::string> unknown{"--not-a-real-flag"};
        index = 0;
        CHECK_FALSE(consume_cli_config_flag(unknown, index, overrides));

        const std::vector<std::string> removedSnyk{"--snyk"};
        index = 0;
        CHECK_FALSE(consume_cli_config_flag(removedSnyk, index, overrides));

        const std::vector<std::string> removedOwasp{"--owasp"};
        index = 0;
        CHECK_FALSE(consume_cli_config_flag(removedOwasp, index, overrides));
    }

    SECTION("define flag maps proxy default target override") {
        const std::vector<std::string> arguments{"-Dproxy.java.default=gradle"};
        std::size_t index = 0;
        ReqPackConfigOverrides overrides;

        REQUIRE(consume_cli_config_flag(arguments, index, overrides));
        REQUIRE(overrides.proxyDefaultTargets.contains("java"));
        CHECK(overrides.proxyDefaultTargets.at("java") == "gradle");
        CHECK(index == 0);
    }

    SECTION("osv and sbom flags map to expected overrides") {
        {
            const std::vector<std::string> arguments{"--osv-refresh", "always"};
            std::size_t index = 0;
            ReqPackConfigOverrides overrides;

            REQUIRE(consume_cli_config_flag(arguments, index, overrides));
            REQUIRE(overrides.osvRefreshMode.has_value());
            CHECK(overrides.osvRefreshMode.value() == OsvRefreshMode::ALWAYS);
        }

        {
            const std::vector<std::string> arguments{"--ignore-vuln", "CVE-2024-1"};
            std::size_t index = 0;
            ReqPackConfigOverrides overrides;

            REQUIRE(consume_cli_config_flag(arguments, index, overrides));
            CHECK(overrides.ignoreVulnerabilityIds == std::vector<std::string>{"CVE-2024-1"});
        }

        {
            const std::vector<std::string> arguments{"--sbom-format", "json"};
            std::size_t index = 0;
            ReqPackConfigOverrides overrides;

            REQUIRE(consume_cli_config_flag(arguments, index, overrides));
            REQUIRE(overrides.sbomDefaultFormat.has_value());
            CHECK(overrides.sbomDefaultFormat.value() == SbomOutputFormat::JSON);
        }

        {
            const std::vector<std::string> arguments{"--sbom-skip-missing-packages"};
            std::size_t index = 0;
            ReqPackConfigOverrides overrides;

            REQUIRE(consume_cli_config_flag(arguments, index, overrides));
            REQUIRE(overrides.sbomSkipMissingPackages.has_value());
            CHECK(overrides.sbomSkipMissingPackages.value());
        }
    }
}

TEST_CASE("configuration extracts multiple CLI overrides in one pass", "[unit][configuration][cli]") {
    std::vector<std::string> arguments{
        "ReqPack",
        "--dry-run",
        "--jobs",
        "6",
        "--registry=/tmp/reqpack-registry",
        "--non-interactive",
        "--osv-db",
        "/tmp/osv-db",
        "--ignore-vuln",
        "CVE-2024-1",
        "-Dproxy.java.default=gradle",
        "--sbom-format",
        "cyclonedx-json",
        "--sbom-skip-missing-packages",
        "--report-format",
        "json",
        "--archive-password",
        "secret",
    };
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }

    const ReqPackConfigOverrides overrides = extract_cli_config_overrides(
        static_cast<int>(argv.size()),
        argv.data()
    );

    REQUIRE(overrides.dryRun.has_value());
    CHECK(overrides.dryRun.value());
    REQUIRE(overrides.jobs.has_value());
    CHECK(overrides.jobs.value() == 6);
    REQUIRE(overrides.jobsMode.has_value());
    CHECK(overrides.jobsMode.value() == ExecutionJobsMode::FIXED);
    REQUIRE(overrides.registryPath.has_value());
    CHECK(overrides.registryPath.value() == "/tmp/reqpack-registry");
    REQUIRE(overrides.interactive.has_value());
    CHECK_FALSE(overrides.interactive.value());
    REQUIRE(overrides.osvDatabasePath.has_value());
    CHECK(overrides.osvDatabasePath.value() == "/tmp/osv-db");
    CHECK(overrides.ignoreVulnerabilityIds == std::vector<std::string>{"CVE-2024-1"});
    REQUIRE(overrides.proxyDefaultTargets.contains("java"));
    CHECK(overrides.proxyDefaultTargets.at("java") == "gradle");
    REQUIRE(overrides.sbomDefaultFormat.has_value());
    CHECK(overrides.sbomDefaultFormat.value() == SbomOutputFormat::CYCLONEDX_JSON);
    REQUIRE(overrides.sbomSkipMissingPackages.has_value());
    CHECK(overrides.sbomSkipMissingPackages.value());
    REQUIRE(overrides.reportFormat.has_value());
    CHECK(overrides.reportFormat.value() == ReportFormat::JSON);
    REQUIRE(overrides.archivePassword.has_value());
    CHECK(overrides.archivePassword.value() == "secret");
}

TEST_CASE("cli parses token vectors and defaults list and outdated to all systems", "[unit][cli][parse]") {
    Cli cli;
    ReqPackConfig config = default_reqpack_config();
    config.registry.pluginDirectory = (repo_root() / "plugins").string();

    SECTION("token vector install parse") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"install", "dnf", "curl", "git"}, config);
        REQUIRE(requests.size() == 1);
        CHECK(requests.front().action == ActionType::INSTALL);
        CHECK(requests.front().system == "dnf");
        CHECK(requests.front().packages == std::vector<std::string>{"curl", "git"});
    }

    SECTION("sys install keeps logical package names that match known systems") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"install", "sys", "java", "maven"}, config);
        REQUIRE(requests.size() == 1);
        CHECK(requests.front().action == ActionType::INSTALL);
        CHECK(requests.front().system == "sys");
        CHECK(requests.front().packages == std::vector<std::string>{"java", "maven"});
    }

    SECTION("sys install still allows explicit scoped system switches") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"install", "sys", "java", "rqp:tool@1.2.3"}, config);
        REQUIRE(requests.size() == 2);
        CHECK(requests[0].action == ActionType::INSTALL);
        CHECK(requests[0].system == "sys");
        CHECK(requests[0].packages == std::vector<std::string>{"java"});
        CHECK(requests[1].action == ActionType::INSTALL);
        CHECK(requests[1].system == "rqp");
        CHECK(requests[1].packages == std::vector<std::string>{"tool@1.2.3"});
    }

    SECTION("sys search keeps known system names as package prompt tokens") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"search", "sys", "java"}, config);
        REQUIRE(requests.size() == 1);
        CHECK(requests.front().action == ActionType::SEARCH);
        CHECK(requests.front().system == "sys");
        CHECK(requests.front().packages == std::vector<std::string>{"java"});
    }

    SECTION("token vector strips proxy define flags from request payload") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"install", "java", "artifact", "-Dproxy.java.default=gradle"}, config);
        REQUIRE(requests.size() == 1);
        CHECK(requests.front().system == "java");
        CHECK(requests.front().packages == std::vector<std::string>{"artifact"});
        CHECK(requests.front().flags.empty());
    }

    SECTION("install accepts jobs flags without forwarding them to plugins") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"install", "dnf", "curl", "--jobs", "3"}, config);
        REQUIRE(requests.size() == 1);
        CHECK(requests.front().packages == std::vector<std::string>{"curl"});
        CHECK(requests.front().flags.empty());
    }

    SECTION("install accepts jobs max without forwarding it to plugins") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"install", "dnf", "curl", "--jobs-max"}, config);
        REQUIRE(requests.size() == 1);
        CHECK(requests.front().packages == std::vector<std::string>{"curl"});
        CHECK(requests.front().flags.empty());
    }

    SECTION("conflicting jobs flags fail in config override extraction layer") {
        const ReqPackConfigOverrides overrides = extract_cli_config_overrides(std::vector<std::string>{"install", "dnf", "curl", "--jobs", "3", "--jobs-max"});
        REQUIRE(overrides.errorMessage.has_value());
        CHECK(overrides.errorMessage.value() == "cannot combine --jobs with --jobs-max");
    }

    SECTION("local regular file before system resolution becomes local target") {
        const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "reqpack-cli-local-target-test.rqp";
        {
            std::ofstream output(tempRoot, std::ios::binary);
            REQUIRE(output.is_open());
            output << "rqp";
        }

        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"install", tempRoot.string()}, config);
        REQUIRE(requests.size() == 1);
        CHECK(requests.front().action == ActionType::INSTALL);
        CHECK(requests.front().usesLocalTarget);
        CHECK(requests.front().localPath == tempRoot.string());
        CHECK(requests.front().system.empty());

        std::error_code error;
        std::filesystem::remove(tempRoot, error);
    }

    SECTION("explicit rqp with local file keeps rqp system") {
        const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "reqpack-cli-local-rqp-test.rqp";
        {
            std::ofstream output(tempRoot, std::ios::binary);
            REQUIRE(output.is_open());
            output << "rqp";
        }

        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"install", "rqp", tempRoot.string()}, config);
        REQUIRE(requests.size() == 1);
        CHECK(requests.front().action == ActionType::INSTALL);
        CHECK(requests.front().system == "rqp");
        CHECK(requests.front().usesLocalTarget);
        CHECK(requests.front().localPath == tempRoot.string());

        std::error_code error;
        std::filesystem::remove(tempRoot, error);
    }

    SECTION("scoped rqp package keeps rqp system and versioned package") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"install", "rqp:tool@1.2.3"}, config);
        REQUIRE(requests.size() == 1);
        CHECK(requests.front().action == ActionType::INSTALL);
        CHECK(requests.front().system == "rqp");
        CHECK(requests.front().packages == std::vector<std::string>{"tool@1.2.3"});
        CHECK_FALSE(requests.front().usesLocalTarget);
    }

    SECTION("list without system targets all known systems") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"list"}, config);
        REQUIRE_FALSE(requests.empty());
        for (const Request& request : requests) {
            CHECK(request.action == ActionType::LIST);
            CHECK_FALSE(request.system.empty());
        }
    }

    SECTION("outdated without system targets all known systems") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"outdated"}, config);
        REQUIRE_FALSE(requests.empty());
        for (const Request& request : requests) {
            CHECK(request.action == ActionType::OUTDATED);
            CHECK_FALSE(request.system.empty());
        }
    }

    SECTION("audit without system targets all known systems") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"audit"}, config);
        REQUIRE_FALSE(requests.empty());
        for (const Request& request : requests) {
            CHECK(request.action == ActionType::AUDIT);
            CHECK_FALSE(request.system.empty());
            CHECK(request.outputFormat == "table");
        }
    }

    SECTION("update without system returns no orchestrator requests for wrapper self-update") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"update"}, config);
        CHECK(requests.empty());
        CHECK_FALSE(cli.parseFailed());
    }

    SECTION("host refresh is handled outside orchestrator request parsing") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"host", "refresh"}, config);
        CHECK(requests.empty());
        CHECK_FALSE(cli.parseFailed());
    }

    SECTION("update all expands to known non-builtin plugins") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"update", "--all"}, config);
        REQUIRE_FALSE(requests.empty());
        for (const Request& request : requests) {
            CHECK(request.action == ActionType::UPDATE);
            CHECK(request.system != "rqp");
            CHECK(request.packages.empty());
            CHECK(std::find(request.flags.begin(), request.flags.end(), "all") != request.flags.end());
            CHECK(std::find(request.flags.begin(), request.flags.end(), "__reqpack-internal-plugin-refresh-all") != request.flags.end());
        }
    }

    SECTION("update all discovers configured sources even before plugin materialization") {
        const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "reqpack-cli-update-all-config-sources";
        std::error_code error;
        std::filesystem::remove_all(tempRoot, error);
        std::filesystem::create_directories(tempRoot / "plugins");
        config.registry.pluginDirectory = (tempRoot / "plugins").string();
        config.registry.databasePath = (tempRoot / "registry-db").string();
        config.registry.sources = {
            {"pip", RegistrySourceEntry{.source = "https://example.test/pip.lua"}},
            {"npm", RegistrySourceEntry{.source = "https://example.test/npm.lua"}},
        };

        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"update", "--all"}, config);
        REQUIRE(requests.size() == 2);
        CHECK(requests[0].system == "npm");
        CHECK(requests[1].system == "pip");
        for (const Request& request : requests) {
            CHECK(request.action == ActionType::UPDATE);
            CHECK(request.packages.empty());
            CHECK(std::find(request.flags.begin(), request.flags.end(), "all") != request.flags.end());
            CHECK(std::find(request.flags.begin(), request.flags.end(), "__reqpack-internal-plugin-refresh-all") != request.flags.end());
        }

        std::filesystem::remove_all(tempRoot, error);
    }

    SECTION("update with explicit system remains normal orchestrator request") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"update", "pip"}, config);
        REQUIRE(requests.size() == 1);
        CHECK(requests.front().action == ActionType::UPDATE);
        CHECK(requests.front().system == "pip");
        CHECK(requests.front().packages.empty());
    }

    SECTION("update system all keeps explicit system-wide package update request") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"update", "pip", "--all"}, config);
        REQUIRE(requests.size() == 1);
        CHECK(requests.front().action == ActionType::UPDATE);
        CHECK(requests.front().system == "pip");
        CHECK(requests.front().packages.empty());
        CHECK(requests.front().flags == std::vector<std::string>{"all"});
    }

    SECTION("update sys pip stays explicit wrapper request") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"update", "sys", "pip"}, config);
        REQUIRE(requests.size() == 1);
        CHECK(requests.front().action == ActionType::UPDATE);
        CHECK(requests.front().system == "sys");
        CHECK(requests.front().packages == std::vector<std::string>{"pip"});
    }

    SECTION("audit infers sarif format from output path") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"audit", "dnf", "curl", "--output", "report.sarif"}, config);
        REQUIRE(requests.size() == 1);
        CHECK(requests.front().action == ActionType::AUDIT);
        CHECK(requests.front().outputPath == "report.sarif");
        CHECK(requests.front().outputFormat == "sarif");
    }

    SECTION("audit preserves table layout flags") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"audit", "dnf", "curl", "--wide", "--no-wrap"}, config);
        REQUIRE(requests.size() == 1);
        CHECK(requests.front().flags == std::vector<std::string>{"wide", "no-wrap"});
    }

    SECTION("sbom preserves table layout flags") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"sbom", "dnf", "curl", "--wide", "--no-wrap"}, config);
        REQUIRE(requests.size() == 1);
        CHECK(requests.front().flags == std::vector<std::string>{"wide", "no-wrap"});
    }

    SECTION("sbom skips missing packages via config flag without forwarding it to plugins") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"sbom", "dnf", "curl", "--sbom-skip-missing-packages"}, config);
        REQUIRE(requests.size() == 1);
        CHECK(requests.front().flags.empty());
    }

    SECTION("install rejects removed provider-specific security flags") {
        CHECK(cli.parse(std::vector<std::string>{"install", "dnf", "curl", "--snyk"}, config).empty());
        CHECK(cli.parseFailed());

        CHECK(cli.parse(std::vector<std::string>{"install", "dnf", "curl", "--owasp"}, config).empty());
        CHECK(cli.parseFailed());
    }

    SECTION("audit rejects invalid format") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"audit", "dnf", "curl", "--format", "xml"}, config);
        CHECK(requests.empty());
        CHECK(cli.parseFailed());
    }

    SECTION("audit supports direct reqpack manifest file input") {
        const std::filesystem::path manifestDir = std::filesystem::temp_directory_path() / "reqpack-cli-audit-manifest";
        std::filesystem::create_directories(manifestDir);
        const std::filesystem::path manifestPath = manifestDir / "reqpack.lua";
        {
            std::ofstream output(manifestPath);
            REQUIRE(output.is_open());
            output << "return { packages = { { system = 'dnf', name = 'curl' }, { system = 'npm', name = 'react', version = '18.3.1' } } }\n";
        }

        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"audit", manifestPath.string()}, config);
        REQUIRE(requests.size() == 2);
        CHECK(requests[0].action == ActionType::AUDIT);
        CHECK(requests[1].action == ActionType::AUDIT);

        std::error_code error;
        std::filesystem::remove_all(manifestDir, error);
    }

    SECTION("search parses repeated arch and type filters") {
        const std::vector<Request> requests = cli.parse(
            std::vector<std::string>{"search", "dnf", "python3", "--arch", "noarch", "--arch", "x86_64", "--type", "doc", "--type", "devel"},
            config
        );
        REQUIRE(requests.size() == 1);
        CHECK(requests.front().action == ActionType::SEARCH);
        CHECK(requests.front().system == "dnf");
        CHECK(requests.front().packages == std::vector<std::string>{"python3"});
        CHECK(requests.front().flags == std::vector<std::string>{"arch=noarch", "arch=x86_64", "type=doc", "type=devel"});
    }

    SECTION("search rejects missing filter values") {
        CHECK(cli.parse(std::vector<std::string>{"search", "dnf", "python3", "--arch"}, config).empty());
        CHECK(cli.parseFailed());
        CHECK(cli.parse(std::vector<std::string>{"search", "dnf", "python3", "--type"}, config).empty());
        CHECK(cli.parseFailed());
    }

    SECTION("list and outdated parse repeated arch and type filters") {
        const std::vector<Request> listed = cli.parse(
            std::vector<std::string>{"list", "dnf", "--arch", "noarch", "--type", "doc", "--type", "devel"},
            config
        );
        REQUIRE(listed.size() == 1);
        CHECK(listed.front().action == ActionType::LIST);
        CHECK(listed.front().system == "dnf");
        CHECK(listed.front().flags == std::vector<std::string>{"arch=noarch", "type=doc", "type=devel"});

        const std::vector<Request> outdated = cli.parse(
            std::vector<std::string>{"outdated", "dnf", "--arch", "x86_64", "--arch", "noarch", "--type", "doc"},
            config
        );
        REQUIRE(outdated.size() == 1);
        CHECK(outdated.front().action == ActionType::OUTDATED);
        CHECK(outdated.front().system == "dnf");
        CHECK(outdated.front().flags == std::vector<std::string>{"arch=x86_64", "arch=noarch", "type=doc"});
    }

    SECTION("list and outdated reject missing filter values") {
        CHECK(cli.parse(std::vector<std::string>{"list", "dnf", "--arch"}, config).empty());
        CHECK(cli.parseFailed());
        CHECK(cli.parse(std::vector<std::string>{"list", "dnf", "--type"}, config).empty());
        CHECK(cli.parseFailed());
        CHECK(cli.parse(std::vector<std::string>{"outdated", "dnf", "--arch"}, config).empty());
        CHECK(cli.parseFailed());
        CHECK(cli.parse(std::vector<std::string>{"outdated", "dnf", "--type"}, config).empty());
        CHECK(cli.parseFailed());
    }
}

TEST_CASE("cli recognizes remote command and prints dedicated help", "[unit][cli][remote]") {
    Cli cli;
    ReqPackConfig config = default_reqpack_config();

    SECTION("remote command does not produce orchestrator requests") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"remote", "dev", "list", "apply"}, config);
        CHECK(requests.empty());
    }

    SECTION("remote help includes profile file guidance") {
        std::ostringstream capture;
        std::streambuf* previous = std::cout.rdbuf(capture.rdbuf());

        char arg0[] = "rqp";
        char arg1[] = "remote";
        char arg2[] = "--help";
        char* argv[] = {arg0, arg1, arg2};

        const bool handled = cli.handleHelp(3, argv);

        std::cout.rdbuf(previous);

        CHECK(handled);
        CHECK(capture.str().find("Usage: rqp remote <profile>") != std::string::npos);
        CHECK(capture.str().find("$XDG_CONFIG_HOME/reqpack/remote.lua") != std::string::npos);
        CHECK(capture.str().find("~/.config/reqpack/remote.lua") != std::string::npos);
    }
}

TEST_CASE("cli general help documents logging flags", "[unit][cli][help]") {
    Cli cli;
    std::ostringstream capture;
    std::streambuf* previous = std::cout.rdbuf(capture.rdbuf());

    char arg0[] = "rqp";
    char arg1[] = "--help";
    char* argv[] = {arg0, arg1};

    const bool handled = cli.handleHelp(2, argv);

    std::cout.rdbuf(previous);

    REQUIRE(handled);
    CHECK(capture.str().find("--structured-log-file <path>") != std::string::npos);
    CHECK(capture.str().find("--log-capture-display") != std::string::npos);
    CHECK(capture.str().find("--log-category <name>") != std::string::npos);
    CHECK(capture.str().find("-v,--verbose") != std::string::npos);
}
