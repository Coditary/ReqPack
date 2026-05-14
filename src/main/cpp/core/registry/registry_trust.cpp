#include "core/registry/registry.h"

#include "registry_internal.h"

#include "core/registry/registry_database_core.h"
#include "plugins/lua_bridge.h"

#include <algorithm>

namespace {

bool contains_write_scope(const std::vector<PluginWriteScope>& scopes, const RegistryWriteScope& expected) {
    return std::find_if(scopes.begin(), scopes.end(), [&](const PluginWriteScope& scope) {
        return scope.kind == expected.kind && scope.value == expected.value;
    }) != scopes.end();
}

bool contains_network_scope(const std::vector<PluginNetworkScope>& scopes, const RegistryNetworkScope& expected) {
    return std::find_if(scopes.begin(), scopes.end(), [&](const PluginNetworkScope& scope) {
        return scope.host == expected.host && scope.scheme == expected.scheme && scope.pathPrefix == expected.pathPrefix;
    }) != scopes.end();
}

}  // namespace

bool Registry::passesThinLayerTrust(const RegistryRecord& record) const {
    if (record.name == registry_internal::BUILTIN_RQ_PLUGIN_ID) {
        return true;
    }

    return registry_record_passes_thin_layer_trust(this->config, record);
}

bool Registry::runtimeMetadataMatchesTrustRecord(const std::string& name, const RegistryRecord& record) const {
    if (!this->config.security.requireThinLayer || record.alias || record.name == registry_internal::BUILTIN_RQ_PLUGIN_ID) {
        return true;
    }

    const auto pluginIt = m_plugins.find(name);
    if (pluginIt == m_plugins.end() || pluginIt->second == nullptr) {
        return false;
    }

    const std::optional<PluginSecurityMetadata> metadata = pluginIt->second->getSecurityMetadata();
    if (!metadata.has_value()) {
        return false;
    }

    if (metadata->role != record.role || metadata->privilegeLevel != record.privilegeLevel) {
        return false;
    }

    for (const std::string& capability : record.capabilities) {
        if (std::find(metadata->capabilities.begin(), metadata->capabilities.end(), capability) == metadata->capabilities.end()) {
            return false;
        }
    }

    for (const std::string& ecosystem : record.ecosystemScopes) {
        if (std::find(metadata->ecosystemScopes.begin(), metadata->ecosystemScopes.end(), ecosystem) == metadata->ecosystemScopes.end()) {
            return false;
        }
    }

    for (const RegistryWriteScope& scope : record.writeScopes) {
        if (!contains_write_scope(metadata->writeScopes, scope)) {
            return false;
        }
    }

    for (const RegistryNetworkScope& scope : record.networkScopes) {
        if (!contains_network_scope(metadata->networkScopes, scope)) {
            return false;
        }
    }

    return true;
}

bool Registry::ensurePluginConstructed(const std::string& name) {
    const std::string resolvedName = this->resolvePluginName(name);
    if (m_plugins.find(resolvedName) != m_plugins.end()) {
        return m_plugins[resolvedName] != nullptr;
    }

    const auto pathIt = m_pluginPaths.find(resolvedName);
    if (pathIt == m_pluginPaths.end()) {
        return false;
    }

    m_plugins[resolvedName] = std::make_unique<LuaBridge>(pathIt->second, this->config);
    if (const std::optional<RegistryRecord> record = this->database.resolveRecord(resolvedName)) {
        if (!this->runtimeMetadataMatchesTrustRecord(resolvedName, record.value())) {
            m_plugins.erase(resolvedName);
            m_states[resolvedName] = PluginState::FAILED;
            return false;
        }
    }
    if (m_states.find(resolvedName) == m_states.end()) {
        m_states[resolvedName] = PluginState::REGISTERED;
    }
    return m_plugins[resolvedName] != nullptr;
}

std::optional<PluginSecurityMetadata> Registry::getPluginSecurityMetadata(const std::string& name) {
    const std::string resolvedName = this->resolvePluginName(name);
    if (m_pluginPaths.find(resolvedName) == m_pluginPaths.end()) {
        if (const std::optional<RegistryRecord> record = this->database.resolveRecord(resolvedName)) {
            if (!this->passesThinLayerTrust(record.value())) {
                m_states[resolvedName] = PluginState::FAILED;
                return std::nullopt;
            }

            RegistryRecord materializedRecord = record.value();
            if (materializedRecord.script.empty() && !materializedRecord.alias) {
                const std::optional<RegistryRecord> refreshed = this->database.refreshRecord(resolvedName);
                if (!refreshed.has_value()) {
                    m_states[resolvedName] = PluginState::FAILED;
                    return std::nullopt;
                }
                materializedRecord = refreshed.value();
            }

            if (!registry_record_matches_expected_hashes(materializedRecord)) {
                m_states[resolvedName] = PluginState::FAILED;
                return std::nullopt;
            }
            this->materializePluginScript(materializedRecord);
            this->scanDirectory(this->config.registry.pluginDirectory);
        }
    }

    if (!this->ensurePluginConstructed(resolvedName)) {
        return std::nullopt;
    }

    auto it = m_plugins.find(resolvedName);
    if (it == m_plugins.end() || it->second == nullptr) {
        return std::nullopt;
    }
    return it->second->getSecurityMetadata();
}

bool Registry::refreshPlugin(const std::string& name, bool preferLatestTag) {
    const std::string resolvedName = this->resolvePluginName(name);
    if (resolvedName == registry_internal::BUILTIN_RQ_PLUGIN_ID) {
        return false;
    }

    const std::optional<RegistryRecord> refreshed = this->database.refreshRecord(resolvedName, preferLatestTag);
    if (!refreshed.has_value()) {
        return false;
    }

    if (!this->passesThinLayerTrust(refreshed.value())) {
        m_states[resolvedName] = PluginState::FAILED;
        return false;
    }
    if (!registry_record_matches_expected_hashes(refreshed.value())) {
        m_states[resolvedName] = PluginState::FAILED;
        return false;
    }

    this->materializePluginScript(refreshed.value());
    m_plugins.erase(resolvedName);
    m_pluginPaths.erase(resolvedName);
    m_states.erase(resolvedName);
    this->scanDirectory(this->config.registry.pluginDirectory);
    return true;
}
