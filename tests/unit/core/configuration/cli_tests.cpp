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
    return reqpack_home_directory().parent_path();
}

}  // namespace

TEST_CASE("configuration applies CLI overrides and expands path fields", "[unit][configuration][cli]") {
    ReqPackConfig base;
    ReqPackConfigOverrides overrides;
    overrides.logLevel = LogLevel::DEBUG;
    overrides.logPattern = "[%l] %v";
    overrides.logFilePath = "~/logs/reqpack.log";
    overrides.fileOutput = true;
    overrides.enableBacktrace = true;
    overrides.backtraceSize = 64;
    overrides.runSnykScan = true;
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
    overrides.enableProxyExpansion = false;
    overrides.registryPath = "~/registry";
    overrides.pluginDirectory = "~/plugins";
    overrides.autoLoadPlugins = false;
    overrides.interactive = false;
    overrides.sbomDefaultFormat = SbomOutputFormat::JSON;
    overrides.sbomDefaultOutputPath = "~/exports/sbom.json";
    overrides.sbomPrettyPrint = false;
    overrides.sbomIncludeDependencyEdges = false;

    const ReqPackConfig config = apply_config_overrides(base, overrides);
    const std::filesystem::path home = reqpack_user_home();

    CHECK(config.logging.level == LogLevel::DEBUG);
    CHECK(config.logging.pattern == "[%l] %v");
    CHECK(config.logging.fileOutput);
    CHECK(std::filesystem::path(config.logging.filePath) == home / "logs/reqpack.log");
    CHECK(config.logging.enableBacktrace);
    CHECK(config.logging.backtraceSize == 64);
    CHECK(config.security.runSnykScan);
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
    CHECK_FALSE(config.planner.enableProxyExpansion);
    CHECK(std::filesystem::path(config.registry.databasePath) == home / "registry");
    CHECK(std::filesystem::path(config.registry.pluginDirectory) == home / "plugins");
    CHECK_FALSE(config.registry.autoLoadPlugins);
    CHECK_FALSE(config.interaction.interactive);
    CHECK(config.sbom.defaultFormat == SbomOutputFormat::JSON);
    CHECK(std::filesystem::path(config.sbom.defaultOutputPath) == home / "exports/sbom.json");
    CHECK_FALSE(config.sbom.prettyPrint);
    CHECK_FALSE(config.sbom.includeDependencyEdges);
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
    }
}

TEST_CASE("configuration extracts multiple CLI overrides in one pass", "[unit][configuration][cli]") {
    std::vector<std::string> arguments{
        "ReqPack",
        "--dry-run",
        "--registry=/tmp/reqpack-registry",
        "--non-interactive",
        "--osv-db",
        "/tmp/osv-db",
        "--ignore-vuln",
        "CVE-2024-1",
        "--sbom-format",
        "cyclonedx-json",
        "--report-format",
        "json",
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
    REQUIRE(overrides.registryPath.has_value());
    CHECK(overrides.registryPath.value() == "/tmp/reqpack-registry");
    REQUIRE(overrides.interactive.has_value());
    CHECK_FALSE(overrides.interactive.value());
    REQUIRE(overrides.osvDatabasePath.has_value());
    CHECK(overrides.osvDatabasePath.value() == "/tmp/osv-db");
    CHECK(overrides.ignoreVulnerabilityIds == std::vector<std::string>{"CVE-2024-1"});
    REQUIRE(overrides.sbomDefaultFormat.has_value());
    CHECK(overrides.sbomDefaultFormat.value() == SbomOutputFormat::CYCLONEDX_JSON);
    REQUIRE(overrides.reportFormat.has_value());
    CHECK(overrides.reportFormat.value() == ReportFormat::JSON);
}

TEST_CASE("cli parses token vectors and defaults list and outdated to all systems", "[unit][cli][parse]") {
    Cli cli;
    ReqPackConfig config = DEFAULT_REQPACK_CONFIG;
    config.registry.pluginDirectory = (repo_root() / "plugins").string();

    SECTION("token vector install parse") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"install", "dnf", "curl", "git"}, config);
        REQUIRE(requests.size() == 1);
        CHECK(requests.front().action == ActionType::INSTALL);
        CHECK(requests.front().system == "dnf");
        CHECK(requests.front().packages == std::vector<std::string>{"curl", "git"});
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
}

TEST_CASE("cli recognizes remote command and prints dedicated help", "[unit][cli][remote]") {
    Cli cli;
    ReqPackConfig config = DEFAULT_REQPACK_CONFIG;

    SECTION("remote command does not produce orchestrator requests") {
        const std::vector<Request> requests = cli.parse(std::vector<std::string>{"remote", "dev", "list", "apply"}, config);
        CHECK(requests.empty());
    }

    SECTION("remote help includes profile file guidance") {
        std::ostringstream capture;
        std::streambuf* previous = std::cout.rdbuf(capture.rdbuf());

        char arg0[] = "ReqPack";
        char arg1[] = "remote";
        char arg2[] = "--help";
        char* argv[] = {arg0, arg1, arg2};

        const bool handled = cli.handleHelp(3, argv);

        std::cout.rdbuf(previous);

        CHECK(handled);
        CHECK(capture.str().find("Usage: ReqPack remote <profile>") != std::string::npos);
        CHECK(capture.str().find("~/.reqpack/remote.lua") != std::string::npos);
    }
}
