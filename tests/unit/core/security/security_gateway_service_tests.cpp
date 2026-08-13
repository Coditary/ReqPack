#include <catch2/catch.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <cstdio>
#include <system_error>

#include "core/security/security_gateway_service.h"
#include "core/security/vulnerability_database.h"
#include "test_helpers.h"

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
}

ReqPackConfig hermetic_security_config(const std::filesystem::path& root) {
    const std::filesystem::path feedPath = root / "osv-feed.json";
    write_file(feedPath, R"([
        {
            "id": "CVE-2026-safe-gateway",
            "modified": "2026-01-01T00:00:00Z",
            "affected": [{
                "package": {"ecosystem": "Debian", "name": "openssl"},
                "versions": ["0.0.0"]
            }]
        }
    ])");

    ReqPackConfig config;
    config.security.osvDatabasePath = (root / "osv-db").string();
    config.security.osvFeedUrl = feedPath.string();
    config.security.osvRefreshMode = OsvRefreshMode::ALWAYS;
    return config;
}

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

}  // namespace

TEST_CASE("security gateway falls back to osv when no provider plugins are known", "[unit][security_gateway_service]") {
    TempDir tempDir{"reqpack-security-gateway-fallback"};
    ReqPackConfig config = hermetic_security_config(tempDir.path());
    SecurityGatewayService gateway(nullptr, nullptr, config);

    const std::vector<ValidationFinding> findings = gateway.ensureEcosystemsReady({"Debian"});
    CHECK(findings.empty());
}

TEST_CASE("security gateway discovers security-provider plugins by role", "[unit][security_gateway_service]") {
    TempDir tempDir{"reqpack-security-gateway-discover"};
    ReqPackConfig config = hermetic_security_config(tempDir.path());

    StaticMetadataProvider metadataProvider;
    metadataProvider.metadata["osv"].role = "security-provider";
    metadataProvider.metadata["osv"].ecosystemScopes = {"Debian"};
    metadataProvider.metadata["dnf"].role = "package-manager";
    metadataProvider.metadata["dnf"].osvEcosystem = "Debian";

    SecurityGatewayService gateway(nullptr, &metadataProvider, config);

    const std::vector<ValidationFinding> findings = gateway.ensureEcosystemsReady({"Debian"});
    CHECK(findings.empty());
}

TEST_CASE("security gateway merges configured backends with discovered security-provider plugins", "[unit][security_gateway_service]") {
    TempDir tempDir{"reqpack-security-gateway-merge"};
    ReqPackConfig config = hermetic_security_config(tempDir.path());
    config.security.gateways["security"].backends = {"owasp"};

    StaticMetadataProvider metadataProvider;
    metadataProvider.metadata["osv"].role = "security-provider";

    SecurityGatewayService gateway(nullptr, &metadataProvider, config);

    const std::vector<ValidationFinding> findings = gateway.ensureEcosystemsReady({"Debian"});
    CHECK(std::any_of(findings.begin(), findings.end(), [](const ValidationFinding& finding) {
        return finding.kind == "sync_warning" && finding.source == "owasp";
    }));
    CHECK_FALSE(std::any_of(findings.begin(), findings.end(), [](const ValidationFinding& finding) {
        return finding.kind == "sync_error" && finding.message == "failed to populate requested security ecosystems";
    }));
}

TEST_CASE("security provider plugin name acts as direct gateway system", "[unit][security_gateway_service]") {
    TempDir tempDir{"reqpack-security-gateway-direct-provider"};
    ReqPackConfig config = hermetic_security_config(tempDir.path());

    StaticMetadataProvider metadataProvider;
    metadataProvider.metadata["snyk"].role = "security-provider";
    metadataProvider.metadata["snyk"].ecosystemScopes = {"Maven"};

    SecurityGatewayService gateway(nullptr, &metadataProvider, config);

    CHECK(gateway.isGatewaySystem("snyk"));
    CHECK(gateway.configuredGatewayNames().contains("snyk"));
}

TEST_CASE("security gateway executes snyk backend for maven ecosystem", "[unit][security_gateway_service]") {
    TempDir tempDir{"reqpack-security-gateway-snyk"};
    const std::filesystem::path apiRoot = tempDir.path() / "snyk-api" / "orgs" / "org-1";
    const std::filesystem::path exportFile = tempDir.path() / "export.csv";
    write_file(apiRoot / "export.json", R"({"export_id":"exp-1"})");
    write_file(apiRoot / "jobs" / "export" / "exp-1.json", R"({"status":"FINISHED"})");
    write_file(apiRoot / "export" / "exp-1.json", std::string{"{"} + "\"download_url\":\"file://" + exportFile.string() + "\"}");
    write_file(exportFile,
        "PROBLEM_ID,PROBLEM_TITLE,CVE,PACKAGE_NAME_AND_VERSION,SEMVER_VULNERABLE_RANGE,ISSUE_SEVERITY,NVD_SCORE,SNYK_CVSS_SCORE,UPDATED_AT,FIXED_IN_VERSION,PRODUCT_NAME\n"
        "SNYK-JAVA-LOG4J-1,Remote code execution,CVE-2026-1,pkg:maven/org.apache.logging.log4j/log4j-core@2.14.1,\"[,2.15.0)\",critical,9.8,9.8,2026-01-01T00:00:00Z,2.15.0,Snyk Open Source\n"
    );

    ReqPackConfig config;
    config.security.indexPath = (tempDir.path() / "security-index").string();
    config.security.cachePath = (tempDir.path() / "security-cache").string();
    config.security.backends["snyk"].apiBaseUrl = "file://" + (tempDir.path() / "snyk-api").string();
    config.security.backends["snyk"].apiVersion = "2024-10-15";
    config.security.backends["snyk"].tokenEnv = "REQPACK_TEST_SNYK_TOKEN";
    config.security.backends["snyk"].orgId = "org-1";
    config.security.backends["snyk"].dataset = "issues";
    config.security.backends["snyk"].refreshMode = OsvRefreshMode::ALWAYS;
    ScopedEnvVar token{"REQPACK_TEST_SNYK_TOKEN", "token-1"};

    StaticMetadataProvider metadataProvider;
    metadataProvider.metadata["snyk"].role = "security-provider";
    metadataProvider.metadata["maven"].osvEcosystem = "Maven";

    SecurityGatewayService gateway(nullptr, &metadataProvider, config);
    const std::vector<ValidationFinding> findings = gateway.executeGatewayRequest(
        ActionType::INSTALL,
        "snyk",
        {Package{.action = ActionType::INSTALL, .system = "snyk", .name = "maven"}}
    );

    CHECK(findings.empty());

    ReqPackConfig dbConfig = config;
    dbConfig.security.osvDatabasePath = (tempDir.path() / "security-index" / "Maven").string();
    VulnerabilityDatabase database(dbConfig);
    REQUIRE(database.ensureReady());
    const std::vector<std::string> ids = database.advisoryIdsForPackage("Maven", "org.apache.logging.log4j:log4j-core");
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == "SNYK-JAVA-LOG4J-1");
}

TEST_CASE("security gateway executes trivy backend for maven ecosystem", "[unit][security_gateway_service]") {
    TempDir tempDir{"reqpack-security-gateway-trivy"};
    const std::filesystem::path trivyRoot = tempDir.path() / "trivy-db";
    const std::filesystem::path helperPath = tempDir.path() / "trivy-helper.sh";
    write_file(trivyRoot / "metadata.json", R"({"Version":2})");
    write_file(trivyRoot / "trivy.db", "fixture");
    write_file(helperPath,
        "#!/bin/sh\n"
        "printf '%s\\n' '{\"ecosystem\":\"Maven\",\"packageName\":\"org.apache.logging.log4j:log4j-core\",\"advisoryId\":\"CVE-2021-44228\",\"aliases\":[\"GHSA-jfh8-c2jp-5v3q\"],\"summary\":\"Remote code execution\",\"description\":\"desc\",\"severity\":\"CRITICAL\",\"score\":10.0,\"references\":[\"https://example.test/CVE-2021-44228\"],\"modified\":\"2021-12-10T00:00:00Z\",\"published\":\"2021-12-10T00:00:00Z\",\"ranges\":[{\"introduced\":\"2.0-beta9\",\"fixed\":\"2.15.0\"}]}'\n"
    );
    REQUIRE(std::system((std::string{"chmod +x "} + helperPath.string()).c_str()) == 0);

    ReqPackConfig config;
    config.security.indexPath = (tempDir.path() / "security-index").string();
    config.security.cachePath = (tempDir.path() / "security-cache").string();
    config.security.backends["trivy"].dbRepositories = {"file://" + trivyRoot.string()};
    config.security.backends["trivy"].helperPath = helperPath.string();
    config.security.backends["trivy"].refreshMode = OsvRefreshMode::ALWAYS;

    StaticMetadataProvider metadataProvider;
    metadataProvider.metadata["trivy"].role = "security-provider";
    metadataProvider.metadata["maven"].osvEcosystem = "Maven";
    metadataProvider.metadata["maven"].versionComparator.profile = "maven-comparable";

    SecurityGatewayService gateway(nullptr, &metadataProvider, config);
    const std::vector<ValidationFinding> findings = gateway.executeGatewayRequest(
        ActionType::INSTALL,
        "trivy",
        {Package{.action = ActionType::INSTALL, .system = "trivy", .name = "maven"}}
    );

    CHECK(findings.empty());

    ReqPackConfig dbConfig = config;
    dbConfig.security.osvDatabasePath = (tempDir.path() / "security-index" / "Maven").string();
    VulnerabilityDatabase database(dbConfig);
    REQUIRE(database.ensureReady());
    const std::vector<std::string> ids = database.advisoryIdsForPackage("Maven", "org.apache.logging.log4j:log4j-core");
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == "CVE-2021-44228");
}

TEST_CASE("security gateway executes gh-advisory backend for arbitrary available ecosystem", "[unit][security_gateway_service]") {
    TempDir tempDir{"reqpack-security-gateway-gh-advisory"};
    const std::filesystem::path advisoryRoot = tempDir.path() / "gh-advisory-db" / "advisories" / "github-reviewed" / "2026" / "01";
    write_file(advisoryRoot / "GHSA-demo.json", R"({
        "id": "GHSA-pip-demo",
        "modified": "2026-01-01T00:00:00Z",
        "summary": "pip advisory",
        "affected": [{
            "package": {"ecosystem": "pip", "name": "urllib3"},
            "versions": ["1.26.18"]
        }]
    })");

    ReqPackConfig config;
    config.security.indexPath = (tempDir.path() / "security-index").string();
    config.security.cachePath = (tempDir.path() / "security-cache").string();
    config.security.backends["gh-advisory"].feedUrl = "file://" + (tempDir.path() / "gh-advisory-db").string();
    config.security.backends["gh-advisory"].refreshMode = OsvRefreshMode::ALWAYS;

    StaticMetadataProvider metadataProvider;
    metadataProvider.metadata["gh-advisory"].role = "security-provider";

    SecurityGatewayService gateway(nullptr, &metadataProvider, config);
    const std::vector<ValidationFinding> findings = gateway.executeGatewayRequest(
        ActionType::INSTALL,
        "gh-advisory",
        {Package{.action = ActionType::INSTALL, .system = "gh-advisory", .name = "pip"}}
    );

    CHECK(findings.empty());

    ReqPackConfig dbConfig = config;
    dbConfig.security.osvDatabasePath = (tempDir.path() / "security-index" / "pip").string();
    VulnerabilityDatabase database(dbConfig);
    REQUIRE(database.ensureReady());
    const std::vector<std::string> ids = database.advisoryIdsForPackage("pip", "urllib3");
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == "GHSA-pip-demo");
}
