#pragma once

#include <vector>
#include <memory>
#include <string>
#include <map>
#include <optional>
#include "plugins/iplugin.h"

enum class PluginState {
    NOT_FOUND,
    REGISTERED,
    ACTIVE,
    FAILED,
    SHUTDOWN
};

class Registry {
private:
    std::map<std::string, std::unique_ptr<IPlugin>> m_plugins;
    std::map<std::string, PluginState> m_states;

public:
    Registry() = default;
    ~Registry();

    void scanDirectory(const std::string& directoryPath);
    bool loadPlugin(const std::string& name);
    void unloadPlugin(const std::string& name);
    void loadAll();
    void shutdownAll();

    
    bool isLoaded(const std::string& name) const;
    PluginState getState(const std::string& name) const;
    std::vector<std::string> findByCategory(const std::string& category) const;
    std::vector<std::string> getAvailableNames() const;

    IPlugin* getPlugin(const std::string& name);
};
