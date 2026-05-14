#include "registry_database_internal.h"

#include "core/download/downloader_core.h"
#include "core/plugins/plugin_bundle.h"
#include "output/diagnostic.h"
#include "output/logger.h"

#include <curl/curl.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::optional<std::pair<std::string, std::string>> read_plugin_payload_files(
    const std::filesystem::path& scriptPath,
    const std::filesystem::path& bootstrapPath
) {
    if (!std::filesystem::exists(scriptPath)) {
        return std::nullopt;
    }

    const std::string script = read_text_file(scriptPath);
    if (!registry_database_is_valid_plugin_script(script)) {
        return std::nullopt;
    }

    return std::make_pair(
        script,
        std::filesystem::exists(bootstrapPath) ? read_text_file(bootstrapPath) : std::string{}
    );
}

std::optional<std::pair<std::string, std::string>> read_plugin_directory(const std::filesystem::path& directory, const std::string& pluginName) {
    if (const std::optional<PluginBundleLayout> layout = plugin_bundle_find_root(directory, pluginName); layout.has_value()) {
        return std::make_pair(read_text_file(layout->runScriptPath), std::string{});
    }
    return read_plugin_payload_files(directory / (pluginName + ".lua"), directory / "bootstrap.lua");
}

std::optional<std::filesystem::path> plugin_bundle_root(const std::filesystem::path& basePath, const std::string& pluginName) {
    if (const std::optional<PluginBundleLayout> layout = plugin_bundle_find_root(basePath, pluginName); layout.has_value()) {
        return layout->rootDir;
    }

    return std::nullopt;
}

std::optional<std::pair<std::string, std::string>> read_plugin_repository(const std::filesystem::path& repositoryPath, const std::string& pluginName) {
    return read_plugin_directory(repositoryPath, pluginName);
}

std::optional<std::pair<std::string, std::string>> fetch_git_plugin_payload(
    const ReqPackConfig& config,
    const std::string& source,
    const std::string& pluginName
) {
    std::string errorDetails;
    if (!sync_git_repository(config, source, pluginName, &errorDetails)) {
        if (!errorDetails.empty()) {
            Logger::instance().diagnostic(make_error_diagnostic(
                "registry",
                "Plugin source sync failed for '" + pluginName + "'",
                "ReqPack could not clone, fetch, or checkout plugin source repository.",
                "Check plugin source URL and ref in registry, then retry.",
                errorDetails,
                pluginName,
                "plugin-source"
            ));
        }
        return std::nullopt;
    }

    return read_plugin_repository(registry_database_git_repository_cache_path(config, source, pluginName), pluginName);
}

std::size_t write_to_string(void* contents, std::size_t size, std::size_t nmemb, void* userp) {
    const std::size_t bytes = size * nmemb;
    auto* buffer = static_cast<std::string*>(userp);
    buffer->append(static_cast<const char*>(contents), bytes);
    return bytes;
}

std::optional<std::string> fetch_text(const ReqPackConfig& config, const std::string& source) {
    if (source.empty()) {
        return std::nullopt;
    }

    if (source.find("://") == std::string::npos) {
        const std::string content = read_text_file(source);
        return content.empty() ? std::nullopt : std::optional<std::string>(content);
    }

    std::string buffer;
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        return std::nullopt;
    }

    downloader_configure_curl_handle(curl, config, source);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

    long statusCode = 0;
    const CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK || statusCode >= 400 || buffer.empty()) {
        return std::nullopt;
    }

    return buffer;
}

}  // namespace

std::optional<std::filesystem::path> resolve_bundle_path(const ReqPackConfig& config, const std::string& source, const std::string& pluginName) {
    const std::filesystem::path sourcePath(source);
    if (std::filesystem::exists(sourcePath) && std::filesystem::is_directory(sourcePath)) {
        if (const auto bundleRoot = plugin_bundle_root(sourcePath, pluginName)) {
            return bundleRoot;
        }
    }

    if (registry_database_is_git_source(source)) {
        const std::filesystem::path repositoryPath = registry_database_git_repository_cache_path(config, source, pluginName);
        if (const auto bundleRoot = plugin_bundle_root(repositoryPath, pluginName)) {
            return bundleRoot;
        }
    }

    return std::nullopt;
}

std::optional<std::pair<std::string, std::string>> fetch_plugin_payload(
    const ReqPackConfig& config,
    const std::string& source,
    const std::string& pluginName
) {
    if (source.empty()) {
        return std::nullopt;
    }

    const std::filesystem::path sourcePath(source);
    if (std::filesystem::exists(sourcePath) && std::filesystem::is_directory(sourcePath)) {
        return read_plugin_directory(sourcePath, pluginName);
    }

    if (registry_database_is_git_source(source)) {
        return fetch_git_plugin_payload(config, source, pluginName);
    }

    if (source.find("://") == std::string::npos) {
        const std::string script = read_text_file(sourcePath);
        if (!registry_database_is_valid_plugin_script(script)) {
            return std::nullopt;
        }

        const std::filesystem::path bootstrapPath = sourcePath.parent_path() / "bootstrap.lua";
        return std::make_pair(
            script,
            std::filesystem::exists(bootstrapPath) ? read_text_file(bootstrapPath) : std::string{}
        );
    }

    const std::optional<std::string> script = fetch_text(config, source);
    if (!script.has_value() || !registry_database_is_valid_plugin_script(script.value())) {
        return std::nullopt;
    }

    return std::make_pair(script.value(), std::string{});
}

std::optional<RegistryRecord> refreshed_record_payload(
    const ReqPackConfig& config,
    RegistryRecord record,
    bool preferLatestTag
) {
    if (record.alias) {
        return record;
    }

    if (!registry_record_passes_thin_layer_trust(config, record)) {
        return std::nullopt;
    }

    std::string sourceForPayload = record.source;
    if (preferLatestTag && registry_database_is_git_source(record.source)) {
        if (const std::optional<std::string> latestTag = latest_git_tag_for_source(record.source)) {
            sourceForPayload = registry_database_git_source_with_ref(record.source, latestTag.value());
        }
    }

    if (const auto fetchedPayload = fetch_plugin_payload(config, sourceForPayload, record.name)) {
        record.script = fetchedPayload->first;
        record.bootstrapScript = fetchedPayload->second;
        if (const auto bundlePath = resolve_bundle_path(config, sourceForPayload, record.name)) {
            record.bundleSource = true;
            record.bundlePath = bundlePath->string();
        } else {
            record.bundleSource = false;
            record.bundlePath.clear();
        }
        if (!registry_record_matches_expected_hashes(record)) {
            return std::nullopt;
        }
        return record;
    }

    return std::nullopt;
}
