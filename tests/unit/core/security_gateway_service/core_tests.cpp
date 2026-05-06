#include <catch2/catch.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <system_error>

#include "core/security_gateway_service.h"

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
