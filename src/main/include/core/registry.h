#pragma once

#include "core/configuration.h"
#include "core/plugin_metadata_provider.h"
#include "core/registry_database.h"

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

class Registry : public PluginMetadataProvider {
private:
    ReqPackConfig config;
    RegistryDatabase database;
    std::map<std::string, std::string> m_pluginPaths;
    std::map<std::string, std::unique_ptr<IPlugin>> m_plugins;
    std::map<std::string, PluginState> m_states;

    void materializePluginScript(const RegistryRecord& record) const;
    void registerBuiltInPlugins();
    bool ensurePluginConstructed(const std::string& name);

public:
    Registry(const ReqPackConfig& config = DEFAULT_REQPACK_CONFIG);
    ~Registry();

    RegistryDatabase* getDatabase();
    const RegistryDatabase* getDatabase() const;
    std::string resolvePluginName(const std::string& name) const;
    std::optional<PluginSecurityMetadata> getPluginSecurityMetadata(const std::string& name);
    std::vector<std::string> getKnownPluginNames() override;

    void scanDirectory(const std::string& directoryPath);
    bool loadPlugin(const std::string& name);
    void unloadPlugin(const std::string& name);
    void loadAll();
    void shutdownAll();

    
    bool isLoaded(const std::string& name) const;
    PluginState getState(const std::string& name) const;
    std::vector<std::string> findByCategory(const std::string& category) const;
    std::vector<std::string> getAvailableNames() const;

    // Returns the plugin name (system) that declares the given file extension,
    // or an empty string if no loaded plugin claims it.
    std::string resolveSystemForExtension(const std::string& extension) const;
    std::string resolveSystemForLocalTarget(const std::filesystem::path& path) const;

    IPlugin* getPlugin(const std::string& name);
};
