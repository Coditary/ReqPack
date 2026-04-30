#include "plugins/rq_plugin.h"

#include "core/rq_package.h"

#include "output/logger.h"
#include "plugins/exec_rules.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <stdlib.h>

#include <sol/sol.hpp>

namespace {

class RqRuntimeHost final : public IPluginRuntimeHost {
public:
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
        Logger::instance().emit(OutputAction::PLUGIN_STATUS, OutputContext{.source = hasItemId ? pluginId : "plugin", .scope = "rq", .statusCode = statusCode});
    }

    void emitProgress(const std::string& pluginId, int percent) override {
        const bool hasItemId = pluginId.find(':') != std::string::npos;
        Logger::instance().emit(OutputAction::PLUGIN_PROGRESS, OutputContext{.source = hasItemId ? pluginId : "plugin", .scope = "rq", .progressPercent = std::clamp(percent, 0, 100)});
    }

    void emitBeginStep(const std::string& pluginId, const std::string& label) override {
        const bool hasItemId = pluginId.find(':') != std::string::npos;
        Logger::instance().emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = hasItemId ? pluginId : "plugin", .scope = "rq", .eventName = "begin_step", .payload = label});
    }

    void emitCommit(const std::string& pluginId) override {
        const bool hasItemId = pluginId.find(':') != std::string::npos;
        Logger::instance().emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = hasItemId ? pluginId : "plugin", .scope = "rq", .eventName = "commit", .payload = "committed"});
    }

    void emitSuccess(const std::string& pluginId) override {
        const bool hasItemId = pluginId.find(':') != std::string::npos;
        Logger::instance().emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = hasItemId ? pluginId : "plugin", .scope = "rq", .eventName = "success", .payload = "ok"});
    }

    void emitFailure(const std::string& pluginId, const std::string& message) override {
        const bool hasItemId = pluginId.find(':') != std::string::npos;
        Logger::instance().emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = hasItemId ? pluginId : "plugin", .scope = "rq", .eventName = "failed", .payload = message});
    }

    void emitEvent(const std::string& pluginId, const std::string& eventName, const std::string& payload) override {
        const bool hasItemId = pluginId.find(':') != std::string::npos;
        Logger::instance().emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = hasItemId ? pluginId : "plugin", .scope = "rq", .eventName = eventName, .payload = payload});
    }

    void registerArtifact(const std::string& pluginId, const std::string& payload) override {
        const bool hasItemId = pluginId.find(':') != std::string::npos;
        Logger::instance().emit(OutputAction::PLUGIN_ARTIFACT, OutputContext{.source = hasItemId ? pluginId : "plugin", .scope = "rq", .payload = payload});
    }

    ExecResult execute(const std::string& pluginId, const std::string& command) override {
        return run_plugin_command(Logger::instance(), pluginId, command);
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

    bool download(const std::string& pluginId, const std::string& url, const std::string& destinationPath) override {
        (void)pluginId;
        (void)url;
        (void)destinationPath;
        return false;
    }

private:
    std::vector<std::filesystem::path> tempDirectories_{};
};

RqRuntimeHost RQ_RUNTIME_HOST;

PackageInfo rq_info_item(const std::string& name, const std::string& version, const std::string& description) {
    PackageInfo info;
    info.name = name;
    info.version = version;
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

}  // namespace

RqPlugin::RqPlugin(const ReqPackConfig& config) : config_(config) {}

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
    return "rq";
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
    return packages;
}

bool RqPlugin::install(const PluginCallContext& context, const std::vector<Package>& packages) {
    (void)context;
    (void)packages;
    return false;
}

bool RqPlugin::installLocal(const PluginCallContext& context, const std::string& path) {
    try {
        context.emitBeginStep("load rqp package");
        const std::filesystem::path stateRoot = std::filesystem::path(config_.rq.statePath);
        const std::filesystem::path workRoot = std::filesystem::temp_directory_path() / "reqpack-rq";
        const RqPackageLayout layout = RqPackageReader::load(path, workRoot, stateRoot);

        context.emitBeginStep("run install hook");
        if (!runHook(context, layout, "install")) {
            return false;
        }

        context.emitBeginStep("persist rq state");
        if (!persistInstalledState(layout)) {
            context.emitFailure("failed to persist rq state");
            return false;
        }

        context.emitSuccess();
        return true;
    } catch (const std::exception& error) {
        context.emitFailure(error.what());
        return false;
    }
}

bool RqPlugin::remove(const PluginCallContext& context, const std::vector<Package>& packages) {
    (void)context;
    (void)packages;
    return false;
}

bool RqPlugin::update(const PluginCallContext& context, const std::vector<Package>& packages) {
    (void)context;
    (void)packages;
    return false;
}

std::vector<PackageInfo> RqPlugin::list(const PluginCallContext& context) {
    (void)context;
    return {};
}

std::vector<PackageInfo> RqPlugin::outdated(const PluginCallContext& context) {
    (void)context;
    return {};
}

std::vector<PackageInfo> RqPlugin::search(const PluginCallContext& context, const std::string& prompt) {
    (void)context;
    return {rq_info_item(prompt.empty() ? "rq" : prompt, "1.0.0", "rq search not implemented yet")};
}

PackageInfo RqPlugin::info(const PluginCallContext& context, const std::string& packageName) {
    (void)context;
    return rq_info_item(packageName, "1.0.0", initialized_ ? "rq built-in plugin" : "rq built-in plugin (inactive)");
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
    fs.set_function("copy", [](const std::string& source, const std::string& destination) {
        std::filesystem::create_directories(std::filesystem::path(destination).parent_path());
        std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
        return true;
    });
    fs.set_function("mkdir", [](const std::string& path) {
        std::filesystem::create_directories(path);
        return true;
    });
    fs.set_function("exists", [](const std::string& path) {
        return std::filesystem::exists(path);
    });
    contextTable["fs"] = fs;

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

bool RqPlugin::persistInstalledState(const RqPackageLayout& layout) const {
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
               << "  \"source\": \"local-file\",\n"
               << "  \"path\": \"" << json_escape(layout.packagePath.string()) << "\",\n"
               << "  \"identity\": \"" << json_escape(layout.identity) << "\"\n"
               << "}\n";
    std::ofstream manifestFile(layout.stateDir / "manifest.json", std::ios::binary | std::ios::trunc);
    if (!manifestFile.is_open()) {
        return false;
    }
    manifestFile << "[]\n";
    return true;
}
