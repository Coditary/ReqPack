#include "core/registry.h"
#include "plugins/rq_plugin.h"
#include "plugins/lua_bridge.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace {

constexpr const char* BUILTIN_RQ_PLUGIN_ID = "rqp";

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

Registry::Registry(const ReqPackConfig& config) : config(config), database(config) {
    registerBuiltInPlugins();
}

void Registry::registerBuiltInPlugins() {
    if (!m_plugins.contains(BUILTIN_RQ_PLUGIN_ID)) {
        m_plugins[BUILTIN_RQ_PLUGIN_ID] = std::make_unique<RqPlugin>(this->config);
    }
    if (!m_states.contains(BUILTIN_RQ_PLUGIN_ID)) {
        m_states[BUILTIN_RQ_PLUGIN_ID] = PluginState::REGISTERED;
    }
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
    if (m_states.find(resolvedName) == m_states.end()) {
        m_states[resolvedName] = PluginState::REGISTERED;
    }
    return m_plugins[resolvedName] != nullptr;
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

    if (normalized == BUILTIN_RQ_PLUGIN_ID) {
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

std::optional<PluginSecurityMetadata> Registry::getPluginSecurityMetadata(const std::string& name) {
    const std::string resolvedName = this->resolvePluginName(name);
    if (m_pluginPaths.find(resolvedName) == m_pluginPaths.end()) {
        if (const std::optional<RegistryRecord> record = this->database.resolveRecord(resolvedName)) {
            this->materializePluginScript(record.value());
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

std::vector<std::string> Registry::getKnownPluginNames() {
    std::vector<std::string> names = this->getAvailableNames();
    for (const auto& [name, _] : this->config.planner.systemAliases) {
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

void Registry::materializePluginScript(const RegistryRecord& record) const {
    if (record.name == BUILTIN_RQ_PLUGIN_ID) {
        return;
    }

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
		if (id == BUILTIN_RQ_PLUGIN_ID) {
			continue;
		}
		m_pluginPaths[id] = filePath.string();
		if (m_states.find(id) == m_states.end() || m_states[id] == PluginState::NOT_FOUND) {
			m_states[id] = PluginState::REGISTERED;
		}
    }
}

bool Registry::loadPlugin(const std::string& name) {
    const std::string resolvedName = this->resolvePluginName(name);
    if (resolvedName == BUILTIN_RQ_PLUGIN_ID) {
        if (!this->ensurePluginConstructed(resolvedName)) {
            return false;
        }
        if (m_plugins.find(resolvedName) == m_plugins.end()) return false;
        if (m_states[resolvedName] == PluginState::ACTIVE) return true;
        if (m_states[resolvedName] == PluginState::FAILED) return false;

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

    if (m_pluginPaths.find(resolvedName) == m_pluginPaths.end()) {
        if (const std::optional<RegistryRecord> record = this->database.resolveRecord(resolvedName)) {
            this->materializePluginScript(record.value());
            this->scanDirectory(this->config.registry.pluginDirectory);
        }
    }

    if (!this->ensurePluginConstructed(resolvedName)) {
        return false;
    }

    if (m_plugins.find(resolvedName) == m_plugins.end()) return false;
    if (m_states[resolvedName] == PluginState::ACTIVE) return true;
    if (m_states[resolvedName] == PluginState::FAILED) return false;
    if (!this->config.registry.autoLoadPlugins) return false;

    if (m_plugins[resolvedName]->getInterfaceVersion() != REQPACK_API_VERSION) {
        m_states[resolvedName] = PluginState::FAILED;
        return false;
    }

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
    const std::string resolvedName = this->resolvePluginName(name);
    if (!this->ensurePluginConstructed(resolvedName)) {
        return nullptr;
    }
    auto it = m_plugins.find(resolvedName);
    return (it != m_plugins.end()) ? it->second.get() : nullptr;
}

std::vector<std::string> Registry::findByCategory(const std::string& category) const {
    std::vector<std::string> found;
    for (const std::string& name : this->getAvailableNames()) {
        IPlugin* plugin = const_cast<Registry*>(this)->getPlugin(name);
        if (plugin == nullptr) {
            continue;
        }
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
    for (const auto& [name, _] : m_plugins) {
        names.push_back(name);
    }
    for (const auto& [name, _] : m_pluginPaths) {
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

std::string Registry::resolveSystemForExtension(const std::string& extension) const {
    for (const std::string& name : this->getAvailableNames()) {
        IPlugin* plugin = const_cast<Registry*>(this)->getPlugin(name);
        if (plugin == nullptr) {
            continue;
        }
        const std::vector<std::string> exts = plugin->getFileExtensions();
        if (std::find(exts.begin(), exts.end(), extension) != exts.end()) {
            return name;
        }
    }
    return {};
}
