#include "core/registry/registry.h"

#include "registry_internal.h"

#include "core/plugins/plugin_bundle.h"

#include <filesystem>
#include <fstream>

namespace {

std::string json_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped.push_back(ch); break;
        }
    }
    return escaped;
}

bool write_text_file(const std::filesystem::path& path, const std::string& content) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }
    output << content;
    return output.good();
}

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

void materialize_script_bundle(const std::filesystem::path& targetDirectory, const RegistryRecord& record) {
    const std::string summary = record.description.empty() ? record.name : record.description;
    remove_directory_contents(targetDirectory);
    (void)write_text_file(targetDirectory / "metadata.json",
        "{\n"
        "  \"formatVersion\": 1,\n"
        "  \"name\": \"" + json_escape(record.name) + "\",\n"
        "  \"version\": \"0.0.0\",\n"
        "  \"summary\": \"" + json_escape(summary) + "\",\n"
        "  \"description\": \"" + json_escape(summary) + "\",\n"
        "  \"license\": \"unknown\"\n"
        "}\n");
    (void)write_text_file(targetDirectory / "reqpack.lua", "return {\n  apiVersion = 1,\n  depends = {}\n}\n");
    (void)write_text_file(targetDirectory / "run.lua", record.script);
    (void)write_text_file(targetDirectory / "scripts" / "install.lua", "return true\n");
    (void)write_text_file(targetDirectory / "scripts" / "remove.lua", "return true\n");
}

}  // namespace

void Registry::materializePluginScript(const RegistryRecord& record) const {
    if (record.name == registry_internal::BUILTIN_RQ_PLUGIN_ID) {
        return;
    }

    if (record.script.empty() && (!record.bundleSource || record.bundlePath.empty())) {
        return;
    }

    const std::filesystem::path targetDirectory = std::filesystem::path(this->config.registry.pluginDirectory) / record.name;
    if (record.bundleSource && !record.bundlePath.empty()) {
        if (const std::optional<PluginBundleLayout> layout = plugin_bundle_find_root(record.bundlePath, record.name); layout.has_value()) {
            remove_directory_contents(targetDirectory);
            copy_directory_contents(layout->rootDir, targetDirectory);
            return;
        }
    }

    if (!record.script.empty()) {
        materialize_script_bundle(targetDirectory, record);
        return;
    }

    if (record.bundleSource && !record.bundlePath.empty() && std::filesystem::exists(record.bundlePath)) {
        remove_directory_contents(targetDirectory);
        copy_directory_contents(record.bundlePath, targetDirectory);
        return;
    }
}

void Registry::scanDirectory(const std::string& path) {
    (void)this->database.ensureReady();

    if (!std::filesystem::exists(path)) {
        return;
    }

    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(path, error)) {
        if (error || !entry.is_directory()) {
            continue;
        }

        const std::optional<PluginBundleLayout> layout = plugin_bundle_read_directory(entry.path());
        if (!layout.has_value()) {
            continue;
        }

        std::string id = layout->metadata.name;
        if (id == registry_internal::BUILTIN_RQ_PLUGIN_ID) {
            continue;
        }
        m_pluginPaths[id] = layout->runScriptPath.string();
        if (m_states.find(id) == m_states.end() || m_states[id] == PluginState::NOT_FOUND) {
            m_states[id] = PluginState::REGISTERED;
        }
    }
}
