#include "core/registry/registry.h"

#include "registry_internal.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace {

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool path_matches_extension(const std::filesystem::path& path, const std::string& extension) {
    const std::string normalizedExtension = to_lower(extension);
    const std::string filename = to_lower(path.filename().string());
    return !normalizedExtension.empty() && filename.size() >= normalizedExtension.size() &&
        filename.compare(filename.size() - normalizedExtension.size(), normalizedExtension.size(), normalizedExtension) == 0;
}

std::string resolve_system_for_file_path(const Registry& registry, const std::filesystem::path& path) {
    for (const std::string& name : registry.getAvailableNames()) {
        IPlugin* plugin = const_cast<Registry&>(registry).getPlugin(name);
        if (plugin == nullptr) {
            continue;
        }

        for (const std::string& extension : plugin->getFileExtensions()) {
            if (path_matches_extension(path, extension)) {
                return name;
            }
        }
    }
    return {};
}

}  // namespace

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

std::string Registry::resolveSystemForLocalTarget(const std::filesystem::path& path) const {
    std::error_code error;
    if (std::filesystem::is_directory(path, error) && !error) {
        if (std::filesystem::exists(path / "reqpack.lua", error) && !error) {
            return registry_internal::BUILTIN_RQ_PLUGIN_ID;
        }

        std::string resolvedSystem;
        for (auto it = std::filesystem::recursive_directory_iterator(path, error);
             it != std::filesystem::recursive_directory_iterator();
             it.increment(error)) {
            if (error || !it->is_regular_file()) {
                continue;
            }

            const std::string candidate = resolve_system_for_file_path(*this, it->path());
            if (candidate.empty()) {
                continue;
            }
            if (resolvedSystem.empty()) {
                resolvedSystem = candidate;
                continue;
            }
            if (resolvedSystem != candidate) {
                return {};
            }
        }
        return resolvedSystem;
    }

    return resolve_system_for_file_path(*this, path);
}
