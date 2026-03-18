#include "core/registry.h"
#include "plugins/lua_bridge.h"
#include <filesystem>
#include <iostream>
#include <algorithm>

namespace fs = std::filesystem;

#include <filesystem>
namespace fs = std::filesystem;

void Registry::scanDirectory(const std::string& path) {
    if (!fs::exists(path)) return;

    for (const auto& entry : fs::recursive_directory_iterator(path)) {
        fs::path filePath = entry.path();
        
        if (filePath.extension() == ".lua") {
            std::string id = filePath.stem().string(); 

            if (m_plugins.find(id) == m_plugins.end()) {
                m_plugins[id] = std::make_unique<LuaBridge>(filePath.string());
                m_states[id] = PluginState::REGISTERED;
            }
        }
    }
}

bool Registry::loadPlugin(const std::string& name) {
    if (m_plugins.find(name) == m_plugins.end()) return false;
    if (m_states[name] == PluginState::ACTIVE) return true;

    if (m_plugins[name]->init()) {
        m_states[name] = PluginState::ACTIVE;
        return true;
    } else {
        m_states[name] = PluginState::FAILED;
        return false;
    }
}

bool Registry::isLoaded(const std::string& name) const {
    auto it = m_states.find(name);
    return (it != m_states.end() && it->second == PluginState::ACTIVE);
}

PluginState Registry::getState(const std::string& name) const {
    auto it = m_states.find(name);
    return (it != m_states.end()) ? it->second : PluginState::NOT_FOUND;
}

IPlugin* Registry::getPlugin(const std::string& name) {
    auto it = m_plugins.find(name);
    return (it != m_plugins.end()) ? it->second.get() : nullptr;
}

std::vector<std::string> Registry::findByCategory(const std::string& category) const {
    std::vector<std::string> found;
    for (const auto& [name, plugin] : m_plugins) {
        auto cats = plugin->getCategories();
        if (std::find(cats.begin(), cats.end(), category) != cats.end()) {
            found.push_back(name);
        }
    }
    return found;
}

void Registry::shutdownAll() {
    for (auto& [name, plugin] : m_plugins) {
        if (m_states[name] == PluginState::ACTIVE) {
            plugin->shutdown();
            m_states[name] = PluginState::SHUTDOWN;
        }
    }
}

Registry::~Registry() {
    shutdownAll();
}

std::vector<std::string> Registry::getAvailableNames() const {
    std::vector<std::string> names;
    for (const auto& [name, plugin] : m_plugins) {
        names.push_back(name);
    }
    return names;
}
