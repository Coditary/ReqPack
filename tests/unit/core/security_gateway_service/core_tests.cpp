#include <catch2/catch.hpp>

#include <map>

#include "core/security_gateway_service.h"

namespace {

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
    ReqPackConfig config;
    SecurityGatewayService gateway(nullptr, nullptr, config);

    const std::vector<ValidationFinding> findings = gateway.ensureEcosystemsReady({"Debian"});
    CHECK(findings.empty());
}

TEST_CASE("security gateway discovers security-provider plugins by role", "[unit][security_gateway_service]") {
    ReqPackConfig config;
    config.security.osvRefreshMode = OsvRefreshMode::MANUAL;

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
    ReqPackConfig config;
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
