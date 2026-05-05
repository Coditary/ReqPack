#include "plugins/rq_plugin.h"

#include "core/archive_resolver.h"
#include "core/registry_database.h"
#include "core/rq_package.h"
#include "core/rq_repository.h"
#include "core/rqp_state_store.h"
#include "core/version_compare.h"

#include "output/logger.h"
#include "plugins/exec_rules.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <curl/curl.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <stdlib.h>

#include <sol/sol.hpp>

namespace {

using boost::property_tree::ptree;

constexpr const char* BUILTIN_RQ_PLUGIN_ID = "rqp";

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::optional<ptree> parse_json_tree(const std::string& json) {
    if (json.empty()) {
        return std::nullopt;
    }

    std::istringstream input(json);
    ptree tree;
    try {
        boost::property_tree::read_json(input, tree);
        return tree;
    } catch (...) {
        return std::nullopt;
    }
}

ArchiveExtractionOptions archive_options_from_config(const ReqPackConfig& config) {
    return ArchiveExtractionOptions{
        .password = resolve_archive_password(config),
        .interactive = config.interaction.interactive,
    };
}

std::size_t write_to_file(void* contents, std::size_t size, std::size_t nmemb, void* userp) {
    return std::fwrite(contents, size, nmemb, static_cast<FILE*>(userp));
}

std::optional<std::filesystem::path> unique_nested_file_with_extension(const std::filesystem::path& root, const std::string& extension) {
    std::optional<std::filesystem::path> match;
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(root, error);
         it != std::filesystem::recursive_directory_iterator();
         it.increment(error)) {
        if (error || !it->is_regular_file()) {
            continue;
        }
        if (it->path().extension() != extension) {
            continue;
        }
        if (match.has_value()) {
            throw std::runtime_error("multiple installable rqp files found in extracted archive");
        }
        match = it->path();
    }
    return match;
}

bool rq_package_installed(const std::filesystem::path& stateRoot, const Package& package) {
    const std::filesystem::path packageRoot = stateRoot / package.name;
    std::error_code error;
    if (!std::filesystem::is_directory(packageRoot, error)) {
        return false;
    }

    const std::string prefix = package.version.empty() ? std::string{} : package.name + "@" + package.version + "-";
    for (const auto& entry : std::filesystem::directory_iterator(packageRoot, error)) {
        if (error || !entry.is_directory()) {
            continue;
        }
        if (prefix.empty()) {
            return true;
        }
        if (entry.path().filename().string().rfind(prefix, 0) == 0) {
            return true;
        }
    }

    return false;
}

void collect_materialized_plugin_scripts(
    const std::filesystem::path& root,
    std::map<std::string, std::filesystem::path>& scriptPaths
) {
    std::error_code error;
    if (!std::filesystem::exists(root, error) || error) {
        return;
    }

    for (auto it = std::filesystem::recursive_directory_iterator(root, error);
         it != std::filesystem::recursive_directory_iterator();
         it.increment(error)) {
        if (error) {
            return;
        }

        const std::filesystem::directory_entry& entry = *it;
        if (!entry.is_regular_file() || entry.path().extension() != ".lua") {
            continue;
        }

        if (entry.path().parent_path().filename() != entry.path().stem()) {
            continue;
        }

        const std::string pluginId = to_lower_copy(entry.path().stem().string());
        if (pluginId == BUILTIN_RQ_PLUGIN_ID) {
            continue;
        }

        scriptPaths[pluginId] = entry.path();
    }
}

std::map<std::string, std::filesystem::path> installed_plugin_script_paths(const ReqPackConfig& config) {
    std::map<std::string, std::filesystem::path> scriptPaths;
    collect_materialized_plugin_scripts(std::filesystem::path(config.registry.pluginDirectory), scriptPaths);

    std::error_code error;
    const std::filesystem::path workspacePluginDirectory = std::filesystem::current_path(error) / "plugins";
    const std::filesystem::path configuredPluginDirectory = std::filesystem::path(config.registry.pluginDirectory);
    if (!error && workspacePluginDirectory != configuredPluginDirectory) {
        collect_materialized_plugin_scripts(workspacePluginDirectory, scriptPaths);
    }

    return scriptPaths;
}

std::string try_read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return {};
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string plugin_version_from_script(const std::filesystem::path& scriptPath) {
    static const std::regex versionPattern(R"(function\s+plugin\.getVersion\s*\(\s*\)\s*return\s*["']([^"']+)["'])");

    const std::string script = try_read_text_file(scriptPath);
    if (script.empty()) {
        return {};
    }

    std::smatch match;
    if (!std::regex_search(script, match, versionPattern) || match.size() < 2) {
        return {};
    }

    return match[1].str();
}

PackageInfo installed_plugin_info(
    const std::string& pluginId,
    const RegistryRecord* record,
    const std::string& version,
    const std::string& fallbackType,
    const std::string& fallbackDescription
) {
    PackageInfo info;
    info.system = BUILTIN_RQ_PLUGIN_ID;
    info.name = pluginId;
    info.packageId = pluginId;
    info.version = version;
    info.status = "installed";
    info.installed = "true";
    info.packageType = fallbackType;

    std::string description = fallbackDescription;
    if (record != nullptr) {
        if (record->alias) {
            info.packageType = "alias";
        } else if (!record->role.empty()) {
            info.packageType = record->role;
        }

        if (!record->description.empty()) {
            description = record->description;
        }

        info.sourceUrl = record->source;
    }

    info.summary = description;
    info.description = description;
    return info;
}

class RqRuntimeHost final : public IPluginRuntimeHost {
public:
    void setConfig(const ReqPackConfig* config) {
        config_ = config;
    }

    void logDebug(const std::string& pluginId, const std::string& message) override {
        Logger::instance().emit(OutputAction::LOG, OutputContext{.level = spdlog::level::debug, .message = message, .source = "plugin", .scope = pluginId});
    }

    void logInfo(const std::string& pluginId, const std::string& message) override {
        Logger::instance().emit(OutputAction::LOG, OutputContext{.level = spdlog::level::info, .message = message, .source = "plugin", .scope = pluginId});
    }

    void logWarn(const std::string& pluginId, const std::string& message) override {
        Logger::instance().emit(OutputAction::LOG, OutputContext{.level = spdlog::level::warn, .message = message, .source = "plugin", .scope = pluginId});
    }

    void logError(const std::string& pluginId, const std::string& message) override {
        Logger::instance().emit(OutputAction::LOG, OutputContext{.level = spdlog::level::err, .message = message, .source = "plugin", .scope = pluginId});
    }

    void emitStatus(const std::string& pluginId, int statusCode) override {
        const bool hasItemId = pluginId.find(':') != std::string::npos;
        Logger::instance().emit(OutputAction::PLUGIN_STATUS, OutputContext{.source = hasItemId ? pluginId : "plugin", .scope = "rqp", .statusCode = statusCode});
    }

    void emitProgress(const std::string& pluginId, const DisplayProgressMetrics& metrics) override {
        const bool hasItemId = pluginId.find(':') != std::string::npos;
        const DisplayProgressMetrics normalized = canonicalize_progress_metrics(metrics);
        if (!normalized.percent.has_value() && !normalized.currentBytes.has_value() && !normalized.totalBytes.has_value() && !normalized.bytesPerSecond.has_value()) {
            return;
        }
        Logger::instance().emit(OutputAction::PLUGIN_PROGRESS, OutputContext{
            .source = hasItemId ? pluginId : "plugin",
            .scope = "rqp",
            .progressPercent = normalized.percent,
            .currentBytes = normalized.currentBytes,
            .totalBytes = normalized.totalBytes,
            .bytesPerSecond = normalized.bytesPerSecond,
        });
    }

    void emitBeginStep(const std::string& pluginId, const std::string& label) override {
        const bool hasItemId = pluginId.find(':') != std::string::npos;
        Logger::instance().emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = hasItemId ? pluginId : "plugin", .scope = "rqp", .eventName = "begin_step", .payload = label});
    }

    void emitCommit(const std::string& pluginId) override {
        const bool hasItemId = pluginId.find(':') != std::string::npos;
        Logger::instance().emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = hasItemId ? pluginId : "plugin", .scope = "rqp", .eventName = "commit", .payload = "committed"});
    }

    void emitSuccess(const std::string& pluginId) override {
        const bool hasItemId = pluginId.find(':') != std::string::npos;
        Logger::instance().emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = hasItemId ? pluginId : "plugin", .scope = "rqp", .eventName = "success", .payload = "ok"});
    }

    void emitFailure(const std::string& pluginId, const std::string& message) override {
        const bool hasItemId = pluginId.find(':') != std::string::npos;
        Logger::instance().emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = hasItemId ? pluginId : "plugin", .scope = "rqp", .eventName = "failed", .payload = message});
    }

    void emitEvent(const std::string& pluginId, const std::string& eventName, const std::string& payload) override {
        const bool hasItemId = pluginId.find(':') != std::string::npos;
        Logger::instance().emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = hasItemId ? pluginId : "plugin", .scope = "rqp", .eventName = eventName, .payload = payload});
    }

    void registerArtifact(const std::string& pluginId, const std::string& payload) override {
        const bool hasItemId = pluginId.find(':') != std::string::npos;
        Logger::instance().emit(OutputAction::PLUGIN_ARTIFACT, OutputContext{.source = hasItemId ? pluginId : "plugin", .scope = "rqp", .payload = payload});
        artifactPayloads_.push_back(payload);
    }

    void clearArtifacts() {
        artifactPayloads_.clear();
    }

    std::vector<std::string> takeArtifacts() {
        std::vector<std::string> payloads = std::move(artifactPayloads_);
        artifactPayloads_.clear();
        return payloads;
    }

    ExecResult execute(const std::string& pluginId, const std::string& command) override {
        return run_plugin_command(Logger::instance(), pluginId, "rqp", command);
    }

    std::string createTempDirectory(const std::string& pluginId) override {
        std::filesystem::path tempDir = std::filesystem::temp_directory_path() / ("reqpack-" + pluginId + "-XXXXXX");
        std::string templateString = tempDir.string();
        std::vector<char> buffer(templateString.begin(), templateString.end());
        buffer.push_back('\0');
        char* created = ::mkdtemp(buffer.data());
        if (created == nullptr) {
            return {};
        }
        tempDirectories_.emplace_back(created);
        return created;
    }

    DownloadResult download(const std::string& pluginId, const std::string& url, const std::string& destinationPath) override {
        (void)pluginId;
        const std::filesystem::path targetPath(destinationPath);
        auto finalize = [&](const std::filesystem::path& path) -> DownloadResult {
            try {
                const ArchiveExtractionOptions options = config_ != nullptr ? archive_options_from_config(*config_) : ArchiveExtractionOptions{};
                (void)extract_archive_in_place(path, options);
            } catch (...) {
                return {};
            }
            return DownloadResult{.success = true, .resolvedPath = path.string()};
        };

        std::error_code error;
        std::filesystem::create_directories(targetPath.parent_path(), error);
        if (error) {
            return {};
        }

        if (url.rfind("file://", 0) == 0) {
            std::filesystem::copy_file(url.substr(7), targetPath, std::filesystem::copy_options::overwrite_existing, error);
            if (error) {
                return {};
            }
            return finalize(targetPath);
        }
        if (url.find("://") == std::string::npos) {
            std::filesystem::copy_file(url, targetPath, std::filesystem::copy_options::overwrite_existing, error);
            if (error) {
                return {};
            }
            return finalize(targetPath);
        }

        const std::filesystem::path tempPath = std::filesystem::path(destinationPath + ".tmp");
        FILE* file = std::fopen(tempPath.string().c_str(), "wb");
        if (file == nullptr) {
            return {};
        }

        CURL* curl = curl_easy_init();
        if (curl == nullptr) {
            std::fclose(file);
            return {};
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "ReqPack/0.1.0");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_to_file);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);

        long statusCode = 0;
        const CURLcode result = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
        curl_easy_cleanup(curl);
        std::fclose(file);

        if (result != CURLE_OK || statusCode >= 400) {
            std::filesystem::remove(tempPath, error);
            return {};
        }

        std::filesystem::rename(tempPath, targetPath, error);
        if (error) {
            std::filesystem::remove(tempPath, error);
            return {};
        }
        return finalize(targetPath);
    }

private:
    const ReqPackConfig* config_{nullptr};
    std::vector<std::filesystem::path> tempDirectories_{};
    std::vector<std::string> artifactPayloads_{};
};

RqRuntimeHost RQ_RUNTIME_HOST;

PackageInfo rq_info_item(const std::string& name, const std::string& version, const std::string& description) {
    PackageInfo info;
    info.name = name;
    info.version = version;
    info.summary = description;
    info.description = description;
    return info;
}

std::string json_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
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

std::string manifest_payload_json(const std::string& type, const std::string& path) {
    std::ostringstream stream;
    stream << "{\"type\": \"" << json_escape(type) << "\", \"path\": \"" << json_escape(path) << "\"}";
    return stream.str();
}

std::string metadata_json_for_lua(const RqMetadata& metadata) {
    std::ostringstream stream;
    stream << "{";
    stream << "\"formatVersion\":" << metadata.formatVersion << ",";
    stream << "\"name\":\"" << json_escape(metadata.name) << "\",";
    stream << "\"version\":\"" << json_escape(metadata.version) << "\",";
    stream << "\"release\":" << metadata.release << ",";
    stream << "\"revision\":" << metadata.revision << ",";
    stream << "\"summary\":\"" << json_escape(metadata.summary) << "\",";
    stream << "\"description\":\"" << json_escape(metadata.description) << "\",";
    stream << "\"license\":\"" << json_escape(metadata.license) << "\",";
    stream << "\"architecture\":\"" << json_escape(metadata.architecture) << "\",";
    stream << "\"vendor\":\"" << json_escape(metadata.vendor) << "\",";
    stream << "\"maintainerEmail\":\"" << json_escape(metadata.maintainerEmail) << "\",";
    stream << "\"url\":\"" << json_escape(metadata.url) << "\"";
    stream << "}";
    return stream.str();
}

std::string shell_quote(const std::string& value) {
    std::string quoted{"'"};
    for (char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted += '\'';
    return quoted;
}

}  // namespace

RqPlugin::RqPlugin(const ReqPackConfig& config) : config_(config) {
    RQ_RUNTIME_HOST.setConfig(&config_);
}

bool RqPlugin::init() {
    initialized_ = true;
    return true;
}

bool RqPlugin::shutdown() {
    initialized_ = false;
    return true;
}

std::string RqPlugin::getName() const {
    return "ReqPack Native Package Manager";
}

std::string RqPlugin::getVersion() const {
    return "1.0.0";
}

std::string RqPlugin::getPluginId() const {
    return "rqp";
}

std::string RqPlugin::getPluginDirectory() const {
    return {};
}

std::string RqPlugin::getScriptPath() const {
    return {};
}

std::string RqPlugin::getBootstrapPath() const {
    return {};
}

IPluginRuntimeHost* RqPlugin::getRuntimeHost() {
    return &RQ_RUNTIME_HOST;
}

bool RqPlugin::supportsResolvePackage() const {
    return true;
}

std::vector<Package> RqPlugin::getRequirements() {
    return {};
}

std::vector<std::string> RqPlugin::getCategories() {
    return {"ReqPack", "Native", "Built-in"};
}

std::vector<std::string> RqPlugin::getFileExtensions() const {
    return {".rqp"};
}

std::vector<Package> RqPlugin::getMissingPackages(const std::vector<Package>& packages) {
    std::vector<Package> missingPackages;
    const std::filesystem::path stateRoot(config_.rqp.statePath);
    for (const Package& package : packages) {
        if (!rq_package_installed(stateRoot, package)) {
            missingPackages.push_back(package);
        }
    }
    return missingPackages;
}

bool RqPlugin::install(const PluginCallContext& context, const std::vector<Package>& packages) {
    recentEvents_.clear();
    const std::vector<RqRepositoryIndex> indexes = loadRepositoryIndexes(context);
    if (indexes.empty()) {
        context.emitFailure("rqp repositories not configured");
        return false;
    }

    bool allInstalled = true;
    for (const Package& package : packages) {
        const std::optional<RqRepositoryPackage> resolvedPackage = rq_repository_resolve_package(
            indexes,
            package.name,
            package.version,
            rq_host_architecture()
        );
        if (!resolvedPackage.has_value()) {
            recentEvents_.push_back(PluginEventRecord{.name = "unavailable", .payload = package.version.empty() ? package.name : package.name + "@" + package.version});
            context.emitFailure("package not found in rqp repositories: " + package.name);
            allInstalled = false;
            continue;
        }

        if (!installResolvedPackage(context, package, resolvedPackage.value())) {
            allInstalled = false;
        }
    }

    return allInstalled;
}

bool RqPlugin::installLocal(const PluginCallContext& context, const std::string& path) {
    recentEvents_.clear();
    std::filesystem::path sourcePath(path);
    std::error_code error;
    if (std::filesystem::is_directory(sourcePath, error) && !error) {
        if (!std::filesystem::exists(sourcePath / "reqpack.lua", error) || error) {
            const std::optional<std::filesystem::path> nestedPackage = unique_nested_file_with_extension(sourcePath, ".rqp");
            if (!nestedPackage.has_value()) {
                context.emitFailure("no installable rqp file found in extracted archive");
                return false;
            }
            sourcePath = nestedPackage.value();
        }
    }
    return installPackagePath(context, sourcePath, "local-file", path);
}

bool RqPlugin::remove(const PluginCallContext& context, const std::vector<Package>& packages) {
    recentEvents_.clear();
    RqpStateStore stateStore(config_);
    bool allRemoved = true;
    for (const Package& package : packages) {
        std::vector<RqpInstalledPackage> matches = stateStore.findInstalled(package.name, package.version);
        if (matches.empty()) {
            continue;
        }
        if (package.version.empty() && matches.size() > 1) {
            context.emitFailure("multiple installed versions found for package: " + package.name);
            allRemoved = false;
            continue;
        }
        std::sort(matches.begin(), matches.end(), [](const RqpInstalledPackage& left, const RqpInstalledPackage& right) {
            return compareInstalledVersions(left, right) > 0;
        });
        if (!removeInstalledPackage(context, matches.front())) {
            allRemoved = false;
        }
    }
    return allRemoved;
}

bool RqPlugin::update(const PluginCallContext& context, const std::vector<Package>& packages) {
    recentEvents_.clear();
    RqpStateStore stateStore(config_);
    bool allUpdated = true;
    for (const Package& package : packages) {
        std::vector<RqpInstalledPackage> matches = stateStore.findInstalled(package.name, package.version);
        if (matches.empty()) {
            continue;
        }
        if (package.version.empty() && matches.size() > 1) {
            context.emitFailure("multiple installed versions found for package: " + package.name);
            allUpdated = false;
            continue;
        }

        const RqpInstalledPackage& installed = matches.front();
        if (installed.source.source != "repository") {
            continue;
        }
        if (config_.rqp.repositories.empty()) {
            continue;
        }

        const std::vector<RqRepositoryIndex> indexes = loadRepositoryIndexes(context);
        const std::optional<RqRepositoryPackage> candidate = rq_repository_resolve_package(
            indexes,
            installed.metadata.name,
            {},
            rq_host_architecture()
        );
        if (!candidate.has_value() || !repositoryCandidateIsNewer(installed, candidate.value())) {
            continue;
        }
        if (!removeInstalledPackage(context, installed)) {
            allUpdated = false;
            continue;
        }
        Package requested;
        requested.name = installed.metadata.name;
        requested.version = installed.metadata.version;
        if (!installResolvedPackage(context, requested, candidate.value())) {
            allUpdated = false;
        }
    }
    return allUpdated;
}

std::vector<PackageInfo> RqPlugin::list(const PluginCallContext& context) {
    (void)context;
    std::vector<PackageInfo> packages;
    std::set<std::string> emittedNames;
    std::map<std::string, std::string> installedVersions;
    std::map<std::string, RegistryRecord> recordsByName;
    std::vector<RegistryRecord> aliasRecords;
    RqpStateStore stateStore(config_);

    RegistryDatabase registryDatabase(config_);
    if (registryDatabase.ensureReady()) {
        for (const RegistryRecord& record : registryDatabase.getAllRecords()) {
            const std::string normalizedName = to_lower_copy(record.name);
            if (normalizedName.empty()) {
                continue;
            }
            if (record.alias) {
                aliasRecords.push_back(record);
                continue;
            }
            recordsByName[normalizedName] = record;
        }
    }

    const auto appendPackage = [&](PackageInfo info) {
        const std::string normalizedName = to_lower_copy(info.name);
        if (normalizedName.empty() || !emittedNames.insert(normalizedName).second) {
            return;
        }
        packages.push_back(std::move(info));
    };

    const RegistryRecord* builtinRecord = nullptr;
    if (const auto builtinIt = recordsByName.find(BUILTIN_RQ_PLUGIN_ID); builtinIt != recordsByName.end()) {
        builtinRecord = &builtinIt->second;
    }

    const std::string builtinVersion = this->getVersion();
    installedVersions[BUILTIN_RQ_PLUGIN_ID] = builtinVersion;
    appendPackage(installed_plugin_info(
        BUILTIN_RQ_PLUGIN_ID,
        builtinRecord,
        builtinVersion,
        "builtin",
        this->getName()
    ));

    for (const RqpInstalledPackage& installed : stateStore.listInstalled()) {
        appendPackage(packageInfoFromInstalled(installed));
    }

    for (const auto& [pluginId, scriptPath] : installed_plugin_script_paths(config_)) {
        const RegistryRecord* record = nullptr;
        if (const auto it = recordsByName.find(pluginId); it != recordsByName.end()) {
            record = &it->second;
        }

        const std::string version = plugin_version_from_script(scriptPath);
        installedVersions[pluginId] = version;
        appendPackage(installed_plugin_info(
            pluginId,
            record,
            version,
            "plugin",
            "Locally installed plugin"
        ));
    }

    for (const RegistryRecord& record : aliasRecords) {
        const std::string normalizedAlias = to_lower_copy(record.name);
        const std::string normalizedTarget = to_lower_copy(record.source);
        if (normalizedAlias.empty() || normalizedTarget.empty()) {
            continue;
        }
        if (!installedVersions.contains(normalizedTarget)) {
            continue;
        }

        appendPackage(installed_plugin_info(
            normalizedAlias,
            &record,
            installedVersions[normalizedTarget],
            "alias",
            record.description.empty() ? "Alias for " + normalizedTarget : record.description
        ));
    }

    std::sort(packages.begin(), packages.end(), [](const PackageInfo& left, const PackageInfo& right) {
        return left.name < right.name;
    });

    for (PackageInfo& package : packages) {
        if (package.description.empty()) {
            package.description = package.summary;
        }
    }

    return packages;
}

std::vector<PackageInfo> RqPlugin::outdated(const PluginCallContext& context) {
    (void)context;
    return {};
}

std::vector<PackageInfo> RqPlugin::search(const PluginCallContext& context, const std::string& prompt) {
    (void)context;
    return {rq_info_item(prompt.empty() ? "rqp" : prompt, "1.0.0", "rqp search not implemented yet")};
}

PackageInfo RqPlugin::info(const PluginCallContext& context, const std::string& packageName) {
    (void)context;
    std::vector<RqpInstalledPackage> matches = RqpStateStore(config_).findInstalled(packageName);
    if (matches.empty()) {
        return {};
    }
    if (matches.size() > 1) {
        return rq_info_item(packageName, {}, "multiple installed versions");
    }
    return packageInfoFromInstalled(matches.front());
}

std::optional<Package> RqPlugin::resolvePackage(const PluginCallContext& context, const Package& package) {
    std::vector<RqpInstalledPackage> matches = RqpStateStore(config_).findInstalled(package.name);
    if (!matches.empty()) {
        std::sort(matches.begin(), matches.end(), compareInstalledVersions);
        Package resolved = package;
        resolved.version = installedVersionString(matches.back().metadata);
        return resolved;
    }

    const std::vector<RqRepositoryIndex> indexes = loadRepositoryIndexes(context);
    const std::optional<RqRepositoryPackage> candidate = rq_repository_resolve_package(
        indexes,
        package.name,
        package.version,
        rq_host_architecture()
    );
    if (!candidate.has_value()) {
        return std::nullopt;
    }

    Package resolved = package;
    resolved.version = candidate->version + "-" + std::to_string(candidate->release) + "+r" + std::to_string(candidate->revision);
    return resolved;
}

std::vector<PluginEventRecord> RqPlugin::takeRecentEvents() {
    std::vector<PluginEventRecord> events = std::move(recentEvents_);
    recentEvents_.clear();
    return events;
}

bool RqPlugin::installPackagePath(
    const PluginCallContext& context,
    const std::filesystem::path& path,
    const std::string& sourceType,
    const std::string& sourceValue,
    const std::string& repository
) {
    try {
        pendingManifest_.clear();
        context.emitBeginStep("load rqp package");
        const std::filesystem::path stateRoot = std::filesystem::path(config_.rqp.statePath);
        const std::filesystem::path workRoot = std::filesystem::temp_directory_path() / "reqpack-rqp";
        const RqPackageLayout layout = RqPackageReader::load(path, workRoot, stateRoot);

        context.emitBeginStep("run install hook");
        if (!runHook(context, layout, "install")) {
            return false;
        }

        context.emitBeginStep("persist rqp state");
        if (!persistInstalledState(layout, sourceType, sourceValue, repository)) {
            context.emitFailure("failed to persist rqp state");
            return false;
        }

        context.emitSuccess();
        return true;
    } catch (const std::exception& error) {
        context.emitFailure(error.what());
        return false;
    }
}

bool RqPlugin::installResolvedPackage(const PluginCallContext& context, const Package& package, const RqRepositoryPackage& resolvedPackage) {
    context.emitBeginStep("resolve rqp repository package");
    const std::filesystem::path localArtifactPath = downloadPackageArtifact(context, resolvedPackage.url);
    if (localArtifactPath.empty()) {
        context.emitFailure("failed to download rqp package artifact");
        return false;
    }

    if (!resolvedPackage.packageSha256.empty()) {
        context.emitBeginStep("verify repository artifact sha256");
        const std::string actualSha256 = sha256Hex(readTextFile(localArtifactPath));
        if (actualSha256 != resolvedPackage.packageSha256) {
            context.emitFailure("repository package sha256 mismatch");
            return false;
        }
    }

    const std::string sourceValue = resolvedPackage.name + "@" + resolvedPackage.version;
    return installPackagePath(context, localArtifactPath, "repository", sourceValue, resolvedPackage.repository);
}

bool RqPlugin::removeInstalledPackage(const PluginCallContext& context, const RqpInstalledPackage& installed) {
    context.emitBeginStep("load installed rqp state");
    if (!runInstalledHook(context, installed, "remove")) {
        return false;
    }
    context.emitBeginStep("remove manifest artifacts");
    if (!removeManifestArtifacts(installed)) {
        context.emitFailure("failed to remove manifest artifacts");
        return false;
    }
    context.emitBeginStep("delete rqp state");
    if (!RqpStateStore(config_).removeInstalledState(installed)) {
        context.emitFailure("failed to delete rqp state");
        return false;
    }
    context.emitSuccess();
    return true;
}

bool RqPlugin::runHook(const PluginCallContext& context, const RqPackageLayout& layout, const std::string& hookKey) const {
    const auto hookIt = layout.hooks.find(hookKey);
    if (hookIt == layout.hooks.end()) {
        return true;
    }

    const std::filesystem::path hookPath = layout.controlDir / hookIt->second;
    if (!std::filesystem::is_regular_file(hookPath)) {
        context.emitFailure("hook file not found: " + hookIt->second);
        return false;
    }

    RQ_RUNTIME_HOST.clearArtifacts();

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::package, sol::lib::table, sol::lib::string, sol::lib::math, sol::lib::os);

    sol::table contextTable = lua.create_table();
    sol::table metadataTable = lua.create_table();
    metadataTable["formatVersion"] = layout.metadata.formatVersion;
    metadataTable["name"] = layout.metadata.name;
    metadataTable["version"] = layout.metadata.version;
    metadataTable["release"] = layout.metadata.release;
    metadataTable["revision"] = layout.metadata.revision;
    metadataTable["summary"] = layout.metadata.summary;
    metadataTable["description"] = layout.metadata.description;
    metadataTable["license"] = layout.metadata.license;
    metadataTable["architecture"] = layout.metadata.architecture;
    metadataTable["vendor"] = layout.metadata.vendor;
    metadataTable["maintainerEmail"] = layout.metadata.maintainerEmail;
    metadataTable["url"] = layout.metadata.url;
    contextTable["metadata"] = metadataTable;

    sol::table paths = lua.create_table();
    paths["controlDir"] = layout.controlDir.string();
    paths["payloadDir"] = layout.payloadDir.string();
    paths["workDir"] = layout.workDir.string();
    paths["stateDir"] = layout.stateDir.string();
    paths["installRoot"] = "/";
    contextTable["paths"] = paths;

    sol::table host = lua.create_table();
    host["arch"] = rq_host_architecture();
    host["os"] = "linux";
    contextTable["host"] = host;

    sol::table log = lua.create_table();
    log.set_function("debug", [context](const std::string& message) { context.logDebug(message); });
    log.set_function("info", [context](const std::string& message) { context.logInfo(message); });
    log.set_function("warn", [context](const std::string& message) { context.logWarn(message); });
    log.set_function("error", [context](const std::string& message) { context.logError(message); });
    contextTable["log"] = log;

    sol::table tx = lua.create_table();
    tx.set_function("begin_step", [context](const std::string& label) { context.emitBeginStep(label); });
    tx.set_function("success", [context]() { context.emitSuccess(); });
    tx.set_function("failed", [context](const std::string& message) { context.emitFailure(message); });
    contextTable["tx"] = tx;

    sol::table exec = lua.create_table();
    exec.set_function("run", [context](const std::string& command) {
        return context.execute(command);
    });
    contextTable["exec"] = exec;
    lua.new_usertype<ExecResult>(
        "ExecResult",
        sol::constructors<ExecResult()>(),
        "success", &ExecResult::success,
        "exitCode", &ExecResult::exitCode,
        "stdout", &ExecResult::stdoutText,
        "stderr", &ExecResult::stderrText
    );
    
    sol::table fs = lua.create_table();
    fs.set_function("copy", [context](const std::string& source, const std::string& destination) {
        std::filesystem::create_directories(std::filesystem::path(destination).parent_path());
        std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
        context.registerArtifact(manifest_payload_json("file", destination));
        return true;
    });
    fs.set_function("mkdir", [context](const std::string& path) {
        std::filesystem::create_directories(path);
        context.registerArtifact(manifest_payload_json("dir", path));
        return true;
    });
    fs.set_function("exists", [](const std::string& path) {
        return std::filesystem::exists(path);
    });
    contextTable["fs"] = fs;

    sol::table artifacts = lua.create_table();
    artifacts.set_function("register_file", [context](const std::string& path) {
        context.registerArtifact(manifest_payload_json("file", path));
        return true;
    });
    artifacts.set_function("register_dir", [context](const std::string& path) {
        context.registerArtifact(manifest_payload_json("dir", path));
        return true;
    });
    artifacts.set_function("register_symlink", [context](const std::string& path) {
        context.registerArtifact(manifest_payload_json("symlink", path));
        return true;
    });
    contextTable["artifacts"] = artifacts;

    lua["context"] = contextTable;

    sol::load_result loadResult = lua.load_file(hookPath.string());
    if (!loadResult.valid()) {
        const sol::error err = loadResult;
        context.emitFailure(err.what());
        return false;
    }

    const sol::protected_function_result result = loadResult();
    if (!result.valid()) {
        const sol::error err = result;
        context.emitFailure(err.what());
        return false;
    }

    std::vector<ManifestEntry> manifest;
    for (const std::string& payload : RQ_RUNTIME_HOST.takeArtifacts()) {
        const auto tree = parse_json_tree(payload);
        if (!tree.has_value()) {
            continue;
        }
        manifest.push_back(ManifestEntry{
            .type = tree->get<std::string>("type", {}),
            .path = tree->get<std::string>("path", {}),
        });
    }
    const_cast<RqPlugin*>(this)->pendingManifest_ = std::move(manifest);

    if (result.return_count() == 0) {
        return true;
    }
    if (result.get_type() == sol::type::boolean) {
        return result.get<bool>();
    }
    return true;
}

bool RqPlugin::runInstalledHook(const PluginCallContext& context, const RqpInstalledPackage& installed, const std::string& hookKey) const {
    const auto hookIt = installed.hooks.find(hookKey);
    if (hookIt == installed.hooks.end()) {
        return true;
    }

    const std::filesystem::path hookPath = installed.stateDir / hookIt->second;
    if (!std::filesystem::is_regular_file(hookPath)) {
        context.emitFailure("hook file not found: " + hookIt->second);
        return false;
    }

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::package, sol::lib::table, sol::lib::string, sol::lib::math, sol::lib::os);

    sol::table contextTable = lua.create_table();
    sol::table metadataTable = lua.create_table();
    metadataTable["formatVersion"] = installed.metadata.formatVersion;
    metadataTable["name"] = installed.metadata.name;
    metadataTable["version"] = installed.metadata.version;
    metadataTable["release"] = installed.metadata.release;
    metadataTable["revision"] = installed.metadata.revision;
    metadataTable["summary"] = installed.metadata.summary;
    metadataTable["description"] = installed.metadata.description;
    metadataTable["license"] = installed.metadata.license;
    metadataTable["architecture"] = installed.metadata.architecture;
    metadataTable["vendor"] = installed.metadata.vendor;
    metadataTable["maintainerEmail"] = installed.metadata.maintainerEmail;
    metadataTable["url"] = installed.metadata.url;
    contextTable["metadata"] = metadataTable;

    sol::table paths = lua.create_table();
    paths["controlDir"] = installed.stateDir.string();
    paths["payloadDir"] = "";
    paths["workDir"] = "";
    paths["stateDir"] = installed.stateDir.string();
    paths["installRoot"] = "/";
    contextTable["paths"] = paths;

    sol::table host = lua.create_table();
    host["arch"] = rq_host_architecture();
    host["os"] = "linux";
    contextTable["host"] = host;

    sol::table log = lua.create_table();
    log.set_function("debug", [context](const std::string& message) { context.logDebug(message); });
    log.set_function("info", [context](const std::string& message) { context.logInfo(message); });
    log.set_function("warn", [context](const std::string& message) { context.logWarn(message); });
    log.set_function("error", [context](const std::string& message) { context.logError(message); });
    contextTable["log"] = log;

    lua["context"] = contextTable;

    sol::load_result loadResult = lua.load_file(hookPath.string());
    if (!loadResult.valid()) {
        const sol::error err = loadResult;
        context.emitFailure(err.what());
        return false;
    }
    const sol::protected_function_result result = loadResult();
    if (!result.valid()) {
        const sol::error err = result;
        context.emitFailure(err.what());
        return false;
    }
    if (result.return_count() == 0) {
        return true;
    }
    if (result.get_type() == sol::type::boolean) {
        return result.get<bool>();
    }
    return true;
}

bool RqPlugin::persistInstalledState(
    const RqPackageLayout& layout,
    const std::string& sourceType,
    const std::string& sourceValue,
    const std::string& repository
) const {
    std::filesystem::create_directories(layout.stateDir / "scripts");
    std::filesystem::copy_file(layout.controlDir / "metadata.json", layout.stateDir / "metadata.json", std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(layout.controlDir / "reqpack.lua", layout.stateDir / "reqpack.lua", std::filesystem::copy_options::overwrite_existing);

    const std::filesystem::path scriptsDir = layout.controlDir / "scripts";
    if (std::filesystem::exists(scriptsDir)) {
        for (const auto& entry : std::filesystem::directory_iterator(scriptsDir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            std::filesystem::copy_file(entry.path(), layout.stateDir / "scripts" / entry.path().filename(), std::filesystem::copy_options::overwrite_existing);
        }
    }

    std::ofstream sourceFile(layout.stateDir / "source.json", std::ios::binary | std::ios::trunc);
    if (!sourceFile.is_open()) {
        return false;
    }
    sourceFile << "{\n"
               << "  \"source\": \"" << json_escape(sourceType) << "\",\n"
               << "  \"path\": \"" << json_escape(sourceValue) << "\",\n"
               << "  \"repository\": \"" << json_escape(repository) << "\",\n"
               << "  \"identity\": \"" << json_escape(layout.identity) << "\"\n"
               << "}\n";
    std::ofstream manifestFile(layout.stateDir / "manifest.json", std::ios::binary | std::ios::trunc);
    if (!manifestFile.is_open()) {
        return false;
    }
    manifestFile << manifestJson(pendingManifest_);
    return true;
}

bool RqPlugin::removeManifestArtifacts(const RqpInstalledPackage& installed) const {
    std::vector<ManifestEntry> manifest = parseManifestJson(readTextFile(installed.manifestPath));
    std::reverse(manifest.begin(), manifest.end());
    std::error_code error;
    for (const ManifestEntry& entry : manifest) {
        const std::filesystem::path path(entry.path);
        if (entry.type == "dir") {
            if (std::filesystem::is_directory(path, error) && std::filesystem::is_empty(path, error)) {
                std::filesystem::remove(path, error);
            }
        } else {
            std::filesystem::remove(path, error);
        }
        if (error) {
            return false;
        }
    }
    return true;
}

std::vector<RqPlugin::ManifestEntry> RqPlugin::parseManifestJson(const std::string& content) {
    std::vector<ManifestEntry> manifest;
    const auto tree = parse_json_tree(content);
    if (!tree.has_value()) {
        return manifest;
    }
    for (const auto& [_, child] : tree.value()) {
        manifest.push_back(ManifestEntry{
            .type = child.get<std::string>("type", {}),
            .path = child.get<std::string>("path", {}),
        });
    }
    return manifest;
}

std::string RqPlugin::manifestJson(const std::vector<ManifestEntry>& manifest) {
    std::ostringstream stream;
    stream << "[\n";
    for (std::size_t index = 0; index < manifest.size(); ++index) {
        const ManifestEntry& entry = manifest[index];
        stream << "  {\"type\": \"" << json_escape(entry.type) << "\", \"path\": \"" << json_escape(entry.path) << "\"}";
        if (index + 1 < manifest.size()) {
            stream << ',';
        }
        stream << "\n";
    }
    stream << "]\n";
    return stream.str();
}

PackageInfo RqPlugin::packageInfoFromInstalled(const RqpInstalledPackage& installed) {
    return PackageInfo{
		.system = "rqp",
        .name = installed.metadata.name,
		.packageId = installed.identity,
        .version = installedVersionString(installed.metadata),
		.status = "installed",
		.installed = "true",
		.summary = installed.metadata.summary,
        .description = installed.metadata.summary.empty() ? installed.metadata.description : installed.metadata.summary,
        .homepage = installed.metadata.url,
		.sourceUrl = installed.metadata.sourceUrl,
		.repository = installed.source.repository,
		.architecture = installed.metadata.architecture,
		.license = installed.metadata.license,
        .author = installed.metadata.vendor,
		.maintainer = installed.metadata.packager,
        .email = installed.metadata.maintainerEmail,
		.publishedAt = installed.metadata.buildDate,
		.dependencies = installed.metadata.depends,
		.provides = installed.metadata.provides,
		.conflicts = installed.metadata.conflicts,
		.replaces = installed.metadata.replaces,
		.tags = installed.metadata.tags,
    };
}

std::string RqPlugin::installedVersionString(const RqMetadata& metadata) {
    return metadata.version + "-" + std::to_string(metadata.release) + "+r" + std::to_string(metadata.revision);
}

int RqPlugin::compareInstalledVersions(const RqpInstalledPackage& left, const RqpInstalledPackage& right) {
    const int versionComparison = version_compare_values(left.metadata.version, right.metadata.version);
    if (versionComparison != 0) {
        return versionComparison;
    }
    if (left.metadata.release != right.metadata.release) {
        return left.metadata.release < right.metadata.release ? -1 : 1;
    }
    if (left.metadata.revision != right.metadata.revision) {
        return left.metadata.revision < right.metadata.revision ? -1 : 1;
    }
    return 0;
}

bool RqPlugin::repositoryCandidateIsNewer(const RqpInstalledPackage& installed, const RqRepositoryPackage& candidate) {
    const int versionComparison = version_compare_values(candidate.version, installed.metadata.version);
    if (versionComparison != 0) {
        return versionComparison > 0;
    }
    if (candidate.release != installed.metadata.release) {
        return candidate.release > installed.metadata.release;
    }
    return candidate.revision > installed.metadata.revision;
}

std::vector<RqRepositoryIndex> RqPlugin::loadRepositoryIndexes(const PluginCallContext& context) const {
    std::vector<RqRepositoryIndex> indexes;
    indexes.reserve(config_.rqp.repositories.size());
    for (const std::string& repository : config_.rqp.repositories) {
        const std::filesystem::path path = downloadPackageArtifact(context, repository);
        if (path.empty()) {
            throw std::runtime_error("failed to load rqp repository index: " + repository);
        }
        indexes.push_back(rq_repository_parse_index(readTextFile(path), repository));
    }
    return indexes;
}

std::string RqPlugin::readTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("failed to open file: " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string RqPlugin::sha256Hex(const std::string& bytes) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest.data());

    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (unsigned char value : digest) {
        stream << std::setw(2) << static_cast<int>(value);
    }
    return stream.str();
}

std::filesystem::path RqPlugin::localPathForUrl(const std::string& url) {
    static constexpr const char* filePrefix = "file://";
    if (url.rfind(filePrefix, 0) == 0) {
        return std::filesystem::path(url.substr(7));
    }
    return std::filesystem::path(url);
}

std::filesystem::path RqPlugin::downloadPackageArtifact(const PluginCallContext& context, const std::string& url) {
    const std::filesystem::path localPath = localPathForUrl(url);
    if (localPath != std::filesystem::path(url) && !is_generic_archive_path(localPath) && !is_archive_wrapper_path(localPath)) {
        return localPath;
    }

    const std::string tempDirectory = context.createTempDirectory();
    if (tempDirectory.empty()) {
        return {};
    }
    const std::filesystem::path sourcePath = (localPath != std::filesystem::path(url)) ? localPath : std::filesystem::path(url);
    const std::string suffix = generic_archive_suffix(sourcePath);
    const std::string wrapper = archive_wrapper_suffix(sourcePath);
    std::string extension;
    if (!wrapper.empty()) {
        const std::string innerSuffix = generic_archive_suffix(sourcePath.stem());
        extension = innerSuffix.empty() ? wrapper : innerSuffix + wrapper;
    } else {
        extension = suffix.empty() ? sourcePath.extension().string() : suffix;
    }
    const std::filesystem::path targetPath = std::filesystem::path(tempDirectory) / (extension.empty() ? "download.rqp" : "download" + extension);
    const DownloadResult download = context.downloadFile(url, targetPath.string());
    if (!download.success) {
        return {};
    }

    std::filesystem::path resolvedPath = download.resolvedPath.empty() ? targetPath : std::filesystem::path(download.resolvedPath);
    if (std::filesystem::is_directory(resolvedPath)) {
        const std::filesystem::path nestedPackage = resolvedPath / "download.rqp";
        if (std::filesystem::is_regular_file(nestedPackage)) {
            return nestedPackage;
        }

        const std::optional<std::filesystem::path> discoveredPackage = unique_nested_file_with_extension(resolvedPath, ".rqp");
        if (discoveredPackage.has_value()) {
            return discoveredPackage.value();
        }
    }
    return resolvedPath;
}
