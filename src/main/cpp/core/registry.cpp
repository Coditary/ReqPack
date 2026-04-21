#include "core/registry.h"
#include "plugins/lua_bridge.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace {

void remove_directory_contents(const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::exists(directory)) {
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) {
            return;
        }
        std::filesystem::remove_all(entry.path(), error);
        if (error) {
            return;
        }
    }
}

void copy_directory_contents(const std::filesystem::path& source, const std::filesystem::path& target) {
    std::error_code error;
    std::filesystem::create_directories(target, error);
    if (error) {
        return;
    }

    for (auto it = std::filesystem::recursive_directory_iterator(source, error); it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (error) {
            return;
        }

        const std::filesystem::directory_entry& entry = *it;

        const std::filesystem::path relativePath = std::filesystem::relative(entry.path(), source, error);
        if (error) {
            return;
        }

        if (!relativePath.empty() && *relativePath.begin() == ".git") {
            if (entry.is_directory()) {
                it.disable_recursion_pending();
            }
            continue;
        }

        const std::filesystem::path targetPath = target / relativePath;
        if (entry.is_directory()) {
            std::filesystem::create_directories(targetPath, error);
            if (error) {
                return;
            }
            continue;
        }

        std::filesystem::create_directories(targetPath.parent_path(), error);
        if (error) {
            return;
        }

        std::filesystem::copy_file(entry.path(), targetPath, std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            return;
        }
    }
}

}  // namespace

Registry::Registry(const ReqPackConfig& config) : config(config), database(config) {}

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

void Registry::materializePluginScript(const RegistryRecord& record) const {
    if (record.script.empty()) {
        return;
    }

    const std::filesystem::path targetDirectory = std::filesystem::path(this->config.registry.pluginDirectory) / record.name;
    if (record.bundleSource && !record.bundlePath.empty() && std::filesystem::exists(record.bundlePath)) {
        remove_directory_contents(targetDirectory);
        copy_directory_contents(record.bundlePath, targetDirectory);
        return;
    }

    const std::filesystem::path targetPath = targetDirectory / (record.name + ".lua");
    std::filesystem::create_directories(targetPath.parent_path());

    std::ofstream stream(targetPath, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return;
    }

    stream << record.script;

    const std::filesystem::path bootstrapPath = targetDirectory / "bootstrap.lua";

    if (!record.bootstrapScript.empty()) {
        std::ofstream bootstrapStream(bootstrapPath, std::ios::binary | std::ios::trunc);
        if (bootstrapStream) {
            bootstrapStream << record.bootstrapScript;
        }
        return;
    }

    std::error_code removeError;
    std::filesystem::remove(bootstrapPath, removeError);
}

void Registry::scanDirectory(const std::string& path) {
    (void)this->database.ensureReady();

    for (const RegistryRecord& record : this->database.getAllRecords()) {
        this->materializePluginScript(record);
    }

    if (!std::filesystem::exists(path)) return;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
		std::filesystem::path filePath = entry.path();

        if (!entry.is_regular_file() || filePath.extension() != ".lua") {
            continue;
        }

        if (filePath.parent_path().filename() != filePath.stem()) {
            continue;
        }

        std::string id = filePath.stem().string();

        if (m_plugins.find(id) == m_plugins.end()) {
            m_plugins[id] = std::make_unique<LuaBridge>(filePath.string());
            m_states[id] = PluginState::REGISTERED;
        }
    }
}

bool Registry::loadPlugin(const std::string& name) {
    const std::string resolvedName = this->resolvePluginName(name);
    if (m_plugins.find(resolvedName) == m_plugins.end()) {
        if (const std::optional<RegistryRecord> record = this->database.resolveRecord(resolvedName)) {
            this->materializePluginScript(record.value());
            this->scanDirectory(this->config.registry.pluginDirectory);
        }
    }

    if (m_plugins.find(resolvedName) == m_plugins.end()) return false;
    if (m_states[resolvedName] == PluginState::ACTIVE) return true;
    if (!this->config.registry.autoLoadPlugins) return false;

    if (m_plugins[resolvedName]->init()) {
        m_states[resolvedName] = PluginState::ACTIVE;
        return true;
    } else {
        m_states[resolvedName] = PluginState::FAILED;
        return false;
    }
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
    auto it = m_plugins.find(this->resolvePluginName(name));
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
