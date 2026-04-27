#include "core/downloader_core.h"

#include <algorithm>
#include <cctype>

std::string downloader_to_lower_copy(const std::string& value) {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return normalized;
}

bool downloader_has_non_whitespace(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return !std::isspace(c);
    });
}

bool downloader_looks_like_html_document(const std::string& value) {
    const std::string prefix = downloader_to_lower_copy(value.substr(0, std::min<std::size_t>(value.size(), 512)));
    return prefix.find("<!doctype html") != std::string::npos ||
           prefix.find("<html") != std::string::npos ||
           prefix.find("<head") != std::string::npos ||
           prefix.find("<body") != std::string::npos;
}

bool downloader_is_valid_plugin_script(const std::string& script) {
    return downloader_has_non_whitespace(script) && !downloader_looks_like_html_document(script);
}

bool downloader_is_remote_source(const std::string& source) {
    return source.find("://") != std::string::npos;
}

std::filesystem::path downloader_temp_path_for_target(const std::filesystem::path& targetPath) {
    return std::filesystem::path(targetPath.string() + ".tmp");
}

std::filesystem::path downloader_plugin_target_path(const ReqPackConfig& config, const std::string& system) {
    return std::filesystem::path(config.registry.pluginDirectory) / system / (system + ".lua");
}
