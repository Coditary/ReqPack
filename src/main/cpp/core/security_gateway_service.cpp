#include "core/security_gateway_service.h"

#include "core/registry.h"
#include "core/vulnerability_database.h"
#include "core/vulnerability_sync_service.h"
#include "core/osv_core.h"

#include <algorithm>
#include <cctype>

namespace {

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string package_index_path_for_ecosystem(const ReqPackConfig& config, const std::string& ecosystem) {
    std::string root = config.security.indexPath;
    const SecurityConfig defaults;
    if (root == defaults.indexPath && config.security.osvDatabasePath != defaults.osvDatabasePath) {
        root = config.security.osvDatabasePath;
    }
    return (std::filesystem::path(root) / ecosystem).string();
}

}  // namespace

SecurityGatewayService::SecurityGatewayService(
    Registry* registry,
    PluginMetadataProvider* metadataProvider,
    const ReqPackConfig& config
) : registry(registry), metadataProvider(metadataProvider), config(config) {}

std::string SecurityGatewayService::normalizeGatewayName(const std::string& gateway) const {
    return to_lower_copy(gateway);
}

std::set<std::string> SecurityGatewayService::configuredGatewayNames() const {
    std::set<std::string> names;
    if (this->config.security.gateways.empty()) {
        names.insert(this->normalizeGatewayName(this->config.security.defaultGateway));
        return names;
    }
    for (const auto& [name, gateway] : this->config.security.gateways) {
        if (gateway.enabled) {
            names.insert(name);
        }
    }
    return names;
}

bool SecurityGatewayService::isGatewaySystem(const std::string& system) const {
    return this->configuredGatewayNames().contains(this->normalizeGatewayName(system));
}

std::set<std::string> SecurityGatewayService::resolveGatewayBackends(const std::string& gateway) const {
    const std::string normalizedGateway = this->normalizeGatewayName(gateway);
    if (this->config.security.gateways.empty()) {
        return {"osv"};
    }
    const auto it = this->config.security.gateways.find(normalizedGateway);
    if (it == this->config.security.gateways.end()) {
        if (normalizedGateway == this->normalizeGatewayName(this->config.security.defaultGateway)) {
            return {"osv"};
        }
        return {};
    }
    if (!it->second.enabled) {
        return {};
    }

    std::set<std::string> backends;
    for (const std::string& backend : it->second.backends) {
        if (!backend.empty()) {
            backends.insert(to_lower_copy(backend));
        }
    }
    return backends;
}

std::optional<std::string> SecurityGatewayService::resolveCanonicalEcosystem(const std::string& target) const {
    if (target.empty()) {
        return std::nullopt;
    }

    const std::string normalizedTarget = to_lower_copy(target);
    if (const auto it = this->config.security.ecosystemMap.find(normalizedTarget); it != this->config.security.ecosystemMap.end()) {
        return it->second;
    }
    if (const auto it = this->config.security.osvEcosystemMap.find(normalizedTarget); it != this->config.security.osvEcosystemMap.end()) {
        return it->second;
    }
    if (this->metadataProvider != nullptr) {
        if (const auto metadata = this->metadataProvider->getPluginSecurityMetadata(normalizedTarget); metadata.has_value() && !metadata->osvEcosystem.empty()) {
            return metadata->osvEcosystem;
        }
    }

    return std::nullopt;
}

std::set<std::string> SecurityGatewayService::resolvePackageEcosystems(const std::vector<Package>& packages) const {
    std::set<std::string> ecosystems;
    for (const Package& package : packages) {
        std::string ecosystem;
        if (this->metadataProvider != nullptr) {
            if (const auto metadata = this->metadataProvider->getPluginSecurityMetadata(package.system); metadata.has_value()) {
                ecosystem = metadata->osvEcosystem;
            }
        }
        if (ecosystem.empty()) {
            if (const auto resolved = this->resolveCanonicalEcosystem(package.system); resolved.has_value()) {
                ecosystem = resolved.value();
            }
        }
        if (!ecosystem.empty()) {
            ecosystems.insert(ecosystem);
        }
    }
    return ecosystems;
}

std::vector<ValidationFinding> SecurityGatewayService::ensureEcosystemsReady(
    const std::set<std::string>& ecosystems,
    const std::string& gateway,
    bool forceRefresh
) const {
    if (ecosystems.empty()) {
        return {};
    }

    const std::string gatewayName = gateway.empty() ? this->config.security.defaultGateway : gateway;
    const std::set<std::string> backends = this->resolveGatewayBackends(gatewayName);
    if (backends.empty()) {
        return {ValidationFinding{
            .kind = "sync_error",
            .source = gatewayName.empty() ? "security" : gatewayName,
            .severity = "high",
            .message = "no configured security backends for gateway '" + gatewayName + "'",
        }};
    }

    std::vector<ValidationFinding> findings;
    bool anySuccess = false;
    for (const std::string& backend : backends) {
        if (backend != "osv") {
            findings.push_back(ValidationFinding{
                .kind = "sync_warning",
                .source = backend,
                .severity = "low",
                .message = "security backend not implemented yet",
            });
            continue;
        }

        ReqPackConfig backendConfig = this->config;
        if (const auto backendIt = this->config.security.backends.find("osv"); backendIt != this->config.security.backends.end()) {
            backendConfig.security.osvFeedUrl = backendIt->second.feedUrl.empty()
                ? backendConfig.security.osvFeedUrl
                : backendIt->second.feedUrl;
            backendConfig.security.osvRefreshMode = backendIt->second.refreshMode;
            backendConfig.security.osvRefreshIntervalSeconds = backendIt->second.refreshIntervalSeconds;
            if (!backendIt->second.overlayPath.empty()) {
                backendConfig.security.osvOverlayPath = backendIt->second.overlayPath;
            }
        }
        if (forceRefresh) {
            backendConfig.security.osvRefreshMode = OsvRefreshMode::ALWAYS;
        }

        std::vector<ValidationFinding> backendFindings;
        for (const std::string& ecosystem : ecosystems) {
            ReqPackConfig ecosystemConfig = backendConfig;
            ecosystemConfig.security.osvDatabasePath = package_index_path_for_ecosystem(backendConfig, ecosystem);
            VulnerabilityDatabase database(ecosystemConfig);
            VulnerabilitySyncService syncService(&database, this->metadataProvider, ecosystemConfig, {ecosystem});
            std::vector<ValidationFinding> syncFindings = syncService.ensureReady();
            const bool failed = std::any_of(syncFindings.begin(), syncFindings.end(), [](const ValidationFinding& finding) {
                return finding.kind == "sync_error";
            });
            if (!failed) {
                anySuccess = true;
            }
            backendFindings.insert(backendFindings.end(), syncFindings.begin(), syncFindings.end());
        }
        findings.insert(findings.end(), backendFindings.begin(), backendFindings.end());
    }

    if (!anySuccess && !ecosystems.empty()) {
        findings.push_back(ValidationFinding{
            .kind = "sync_error",
            .source = gatewayName,
            .severity = "high",
            .message = "failed to populate requested security ecosystems",
        });
    }

    return findings;
}

std::vector<ValidationFinding> SecurityGatewayService::executeGatewayRequest(
    ActionType action,
    const std::string& gateway,
    const std::vector<Package>& packages
) const {
    if (action != ActionType::INSTALL && action != ActionType::UPDATE && action != ActionType::ENSURE) {
        return {ValidationFinding{
            .kind = "sync_error",
            .source = gateway,
            .severity = "high",
            .message = "unsupported security gateway action",
        }};
    }

    std::set<std::string> ecosystems;
    for (const Package& package : packages) {
        if (const auto resolved = this->resolveCanonicalEcosystem(package.name); resolved.has_value()) {
            ecosystems.insert(resolved.value());
        }
    }
    return this->ensureEcosystemsReady(ecosystems, gateway, action == ActionType::UPDATE);
}
