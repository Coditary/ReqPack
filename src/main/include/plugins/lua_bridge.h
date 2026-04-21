#pragma once

#include <sol/sol.hpp>
#include <string>
#include <vector>
#include "core/types.h"
#include "plugins/iplugin.h"

class LuaBridge : public IPlugin {
private:
    sol::state m_lua;
    std::string m_name;
    std::string m_version;
    std::string m_pluginId;
    std::string m_pluginDirectory;
    std::string m_scriptPath;
    std::string m_bootstrapPath;
    
    sol::table m_pluginTable;

public:
    LuaBridge(const std::string& scriptPath);
    virtual ~LuaBridge() = default;

    bool init() override;
    bool shutdown() override;

    std::string getName() const override { return m_name; }
    std::string getVersion() const override { return m_version; }
    
    std::vector<Package> getRequirements() override;
    std::vector<std::string> getCategories() override;

    void install(const std::vector<Package>& packages) override;
    void remove(const std::vector<Package>& packages) override;
    void update(const std::vector<Package>& packages) override;

    std::vector<PackageInfo> list() override;
    std::vector<PackageInfo> search(const std::string& prompt) override;
    PackageInfo info(const std::string& packageName) override;
};
