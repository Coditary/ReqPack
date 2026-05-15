#include "core/registry/registry.h"

#include "registry_internal.h"

#include "core/registry/registry_database_core.h"

bool Registry::loadPlugin(const std::string& name) {
    const std::string resolvedName = this->resolvePluginName(name);
    if (resolvedName == registry_internal::BUILTIN_RQ_PLUGIN_ID) {
        if (!this->ensurePluginConstructed(resolvedName)) {
            return false;
        }
        if (m_plugins.find(resolvedName) == m_plugins.end()) {
            return false;
        }
        if (m_states[resolvedName] == PluginState::ACTIVE) {
            return true;
        }
        if (m_states[resolvedName] == PluginState::FAILED) {
            return false;
        }

        if (m_plugins[resolvedName]->getInterfaceVersion() != REQPACK_API_VERSION) {
            m_states[resolvedName] = PluginState::FAILED;
            return false;
        }

        if (m_plugins[resolvedName]->init()) {
            m_states[resolvedName] = PluginState::ACTIVE;
            return true;
        }

        m_states[resolvedName] = PluginState::FAILED;
        return false;
    }

    if (const std::optional<RegistryRecord> record = this->database.resolveRecord(resolvedName)) {
        if (!registry_record_can_materialize_plugin(record.value())) {
            return false;
        }
    }

    if (m_pluginPaths.find(resolvedName) == m_pluginPaths.end()) {
        if (const std::optional<RegistryRecord> record = this->database.resolveRecord(resolvedName)) {
            if (!this->passesThinLayerTrust(record.value())) {
                m_states[resolvedName] = PluginState::FAILED;
                return false;
            }

            RegistryRecord materializedRecord = record.value();
            if (materializedRecord.script.empty() && !materializedRecord.alias) {
                const std::optional<RegistryRecord> refreshed = this->database.refreshRecord(resolvedName);
                if (!refreshed.has_value()) {
                    m_states[resolvedName] = PluginState::FAILED;
                    return false;
                }
                materializedRecord = refreshed.value();
            }

            if (!registry_record_matches_expected_hashes(materializedRecord)) {
                m_states[resolvedName] = PluginState::FAILED;
                return false;
            }
            this->materializePluginScript(materializedRecord);
            this->scanDirectory(this->config.registry.pluginDirectory);
        }
    }

    if (!this->ensurePluginConstructed(resolvedName)) {
        return false;
    }

    if (m_plugins.find(resolvedName) == m_plugins.end()) {
        return false;
    }
    if (m_states[resolvedName] == PluginState::ACTIVE) {
        return true;
    }
    if (m_states[resolvedName] == PluginState::FAILED) {
        return false;
    }
    if (!this->config.registry.autoLoadPlugins) {
        return false;
    }

    if (m_plugins[resolvedName]->getInterfaceVersion() != REQPACK_API_VERSION) {
        m_states[resolvedName] = PluginState::FAILED;
        return false;
    }

    if (m_plugins[resolvedName]->init()) {
        m_states[resolvedName] = PluginState::ACTIVE;
        return true;
    }

    m_states[resolvedName] = PluginState::FAILED;
    return false;
}

void Registry::unloadPlugin(const std::string& name) {
    const std::string resolvedName = this->resolvePluginName(name);
    auto it = m_plugins.find(resolvedName);
    if (it != m_plugins.end()) {
        if (m_states[resolvedName] == PluginState::ACTIVE && it->second != nullptr) {
            it->second->shutdown();
        }
        m_plugins.erase(it);
    }

    m_pluginPaths.erase(resolvedName);
    m_states.erase(resolvedName);
}

bool Registry::isLoaded(const std::string& name) const {
    auto it = m_states.find(this->resolvePluginName(name));
    return (it != m_states.end() && it->second == PluginState::ACTIVE);
}

PluginState Registry::getState(const std::string& name) const {
    auto it = m_states.find(this->resolvePluginName(name));
    return (it != m_states.end()) ? it->second : PluginState::NOT_FOUND;
}

IPlugin* Registry::getPlugin(const std::string& name) {
    const std::string resolvedName = this->resolvePluginName(name);
    if (!this->ensurePluginConstructed(resolvedName)) {
        return nullptr;
    }
    auto it = m_plugins.find(resolvedName);
    return (it != m_plugins.end()) ? it->second.get() : nullptr;
}

void Registry::shutdownAll() {
    if (!this->config.registry.shutDownPluginsOnExit) {
        return;
    }

    for (auto& [name, plugin] : m_plugins) {
        if (m_states[name] == PluginState::ACTIVE) {
            plugin->shutdown();
            m_states[name] = PluginState::SHUTDOWN;
        }
    }
}
