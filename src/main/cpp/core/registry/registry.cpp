#include "core/registry/registry.h"

#include "registry_internal.h"

#include "plugins/rqp_plugin.h"

#include <algorithm>
#include <cctype>

Registry::Registry(const ReqPackConfig& config) : config(config), database(config) {
    registerBuiltInPlugins();
}

void Registry::registerBuiltInPlugins() {
    if (!m_plugins.contains(registry_internal::BUILTIN_RQ_PLUGIN_ID)) {
        m_plugins[registry_internal::BUILTIN_RQ_PLUGIN_ID] = std::make_unique<RqpPlugin>(this->config);
    }
    if (!m_states.contains(registry_internal::BUILTIN_RQ_PLUGIN_ID)) {
        m_states[registry_internal::BUILTIN_RQ_PLUGIN_ID] = PluginState::REGISTERED;
    }
}

RegistryDatabase* Registry::getDatabase() {
    return &this->database;
}

const RegistryDatabase* Registry::getDatabase() const {
    return &this->database;
}

std::string Registry::resolvePluginName(const std::string& name) const {
    auto normalized = name;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (normalized == registry_internal::BUILTIN_RQ_PLUGIN_ID) {
        return normalized;
    }

    if (const std::optional<RegistryRecord> record = this->database.getRecord(normalized)) {
        if (record->alias && !record->source.empty()) {
            return record->source;
        }
    }

    auto alias = this->config.planner.systemAliases.find(normalized);
    if (alias != this->config.planner.systemAliases.end()) {
        return alias->second;
    }

    return normalized;
}

std::vector<std::string> Registry::getKnownPluginNames() {
    std::vector<std::string> names = this->getAvailableNames();
    for (const auto& [name, _] : this->config.planner.systemAliases) {
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

Registry::~Registry() {
    shutdownAll();
}
