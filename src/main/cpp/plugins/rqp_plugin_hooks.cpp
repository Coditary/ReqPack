#include "rqp_plugin_internal.h"

#include "core/archive/archive_resolver.h"
#include "core/common/network_environment.h"

#include "output/logger.h"
#include "plugins/exec_rules.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <curl/curl.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sol/sol.hpp>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using boost::property_tree::ptree;

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

std::string manifest_payload_json(const std::string& type, const std::string& path) {
    std::ostringstream stream;
    stream << "{\"type\": \"" << type << "\", \"path\": \"" << path << "\"}";
    return stream.str();
}

class RqpRuntimeHost final : public IPluginRuntimeHost {
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
        const std::string userAgent = reqpack_user_agent();
        curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
        reqpack_apply_curl_ca_bundle(curl);
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

RqpRuntimeHost RQP_RUNTIME_HOST;

}  // namespace

IPluginRuntimeHost* rqp_plugin_runtime_host() {
    return &RQP_RUNTIME_HOST;
}

void rqp_plugin_configure_runtime_host(const ReqPackConfig* config) {
    RQP_RUNTIME_HOST.setConfig(config);
}

void rqp_plugin_clear_runtime_host_artifacts() {
    RQP_RUNTIME_HOST.clearArtifacts();
}

std::vector<std::string> rqp_plugin_take_runtime_host_artifacts() {
    return RQP_RUNTIME_HOST.takeArtifacts();
}

bool RqpPlugin::runHook(const PluginCallContext& context, const RqPackageLayout& layout, const std::string& hookKey) const {
    const auto hookIt = layout.hooks.find(hookKey);
    if (hookIt == layout.hooks.end()) {
        return true;
    }

    const std::filesystem::path hookPath = layout.controlDir / hookIt->second;
    if (!std::filesystem::is_regular_file(hookPath)) {
        context.emitFailure("hook file not found: " + hookIt->second);
        return false;
    }

    rqp_plugin_clear_runtime_host_artifacts();

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
    metadataTable["system"] = rq_join_systems(layout.metadata.systems);
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
    for (const std::string& payload : rqp_plugin_take_runtime_host_artifacts()) {
        const auto tree = parse_json_tree(payload);
        if (!tree.has_value()) {
            continue;
        }
        manifest.push_back(ManifestEntry{
            .type = tree->get<std::string>("type", {}),
            .path = tree->get<std::string>("path", {}),
        });
    }
    const_cast<RqpPlugin*>(this)->pendingManifest_ = std::move(manifest);

    if (result.return_count() == 0) {
        return true;
    }
    if (result.get_type() == sol::type::boolean) {
        return result.get<bool>();
    }
    return true;
}

bool RqpPlugin::runInstalledHook(const PluginCallContext& context, const RqpInstalledPackage& installed, const std::string& hookKey) const {
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
    metadataTable["system"] = rq_join_systems(installed.metadata.systems);
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
