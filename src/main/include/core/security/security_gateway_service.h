#pragma once

#include "core/config/configuration.h"
#include "core/plugins/plugin_metadata_provider.h"
#include "core/security/validator_core.h"

#include <set>
#include <string>
#include <vector>

class Registry;

class SecurityGatewayService {
    Registry* registry;
    PluginMetadataProvider* metadataProvider;
    ReqPackConfig config;

    std::string normalizeGatewayName(const std::string& gateway) const;
    std::set<std::string> discoverSecurityProviders() const;
    std::set<std::string> resolveGatewayBackends(const std::string& gateway) const;
    std::optional<std::string> resolveCanonicalEcosystem(const std::string& target) const;

public:
    SecurityGatewayService(
        Registry* registry,
        PluginMetadataProvider* metadataProvider = nullptr,
        const ReqPackConfig& config = default_reqpack_config()
    );

    bool isGatewaySystem(const std::string& system) const;
    std::set<std::string> configuredGatewayNames() const;
    std::set<std::string> resolvePackageEcosystems(const std::vector<Package>& packages) const;
    std::vector<ValidationFinding> ensureEcosystemsReady(
        const std::set<std::string>& ecosystems,
        const std::string& gateway = {},
        bool forceRefresh = false
    ) const;
    std::vector<ValidationFinding> executeGatewayRequest(
        ActionType action,
        const std::string& gateway,
        const std::vector<Package>& packages
    ) const;
};
