#include "downloader_plugin_internal.h"

#include "core/plugins/plugin_bundle.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

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

bool write_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file == nullptr) {
        return false;
    }

    const std::size_t written = std::fwrite(content.data(), 1, content.size(), file);
    std::fclose(file);
    return written == content.size();
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

    for (auto it = std::filesystem::recursive_directory_iterator(source, error);
         it != std::filesystem::recursive_directory_iterator();
         it.increment(error)) {
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

namespace downloader_plugin_internal {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

bool write_script_bundle(
    const std::filesystem::path& targetDirectory,
    const std::string& pluginName,
    const std::string& description,
    const std::string& script
) {
    const std::string summary = description.empty() ? pluginName : description;
    remove_directory_contents(targetDirectory);
    return write_file(targetDirectory / "metadata.json",
               "{\n"
               "  \"formatVersion\": 1,\n"
               "  \"name\": \"" + json_escape(pluginName) + "\",\n"
               "  \"version\": \"0.0.0\",\n"
               "  \"summary\": \"" + json_escape(summary) + "\",\n"
               "  \"description\": \"" + json_escape(summary) + "\",\n"
               "  \"license\": \"unknown\"\n"
               "}\n") &&
           write_file(targetDirectory / "reqpack.lua", "return {\n  apiVersion = 1,\n  depends = {}\n}\n") &&
           write_file(targetDirectory / "run.lua", script) &&
           write_file(targetDirectory / "scripts" / "install.lua", "return true\n") &&
           write_file(targetDirectory / "scripts" / "remove.lua", "return true\n");
}

bool materialize_script_record_bundle(
    const std::filesystem::path& targetDirectory,
    const std::string& pluginName,
    const RegistryRecord& record
) {
    if (record.bundleSource && !record.bundlePath.empty() && std::filesystem::exists(record.bundlePath)) {
        if (const std::optional<PluginBundleLayout> layout = plugin_bundle_find_root(record.bundlePath, pluginName); layout.has_value()) {
            remove_directory_contents(targetDirectory);
            copy_directory_contents(layout->rootDir, targetDirectory);
            return true;
        }
    }

    return write_script_bundle(targetDirectory, pluginName, record.description, record.script);
}

}  // namespace downloader_plugin_internal
