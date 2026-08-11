#include "plugin_test_runner_internal.h"

#include <sol/sol.hpp>

#include "core/host/host_info.h"
#include "core/plugins/plugin_bundle.h"
#include "plugins/lua_bridge.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace {

std::string to_lower_copy(const std::string& value) {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return normalized;
}

std::optional<std::string> optional_string_field(const sol::table& table, const char* key) {
    const sol::optional<std::string> value = table[key];
    if (!value.has_value() || value->empty()) {
        return std::nullopt;
    }
    return value.value();
}

std::vector<std::string> string_list_from_table(const sol::object& object) {
    if (!object.valid() || object.is<sol::lua_nil_t>() || object.get_type() != sol::type::table) {
        return {};
    }
    std::vector<std::string> values;
    for (const auto& [_, entry] : object.as<sol::table>()) {
        if (entry.get_type() == sol::type::string) {
            values.push_back(entry.as<std::string>());
        }
    }
    return values;
}

ActionType action_from_string(const std::string& action) {
    const std::string normalized = to_lower_copy(action);
    if (normalized == "install") {
        return ActionType::INSTALL;
    }
    if (normalized == "installlocal") {
        return ActionType::INSTALL;
    }
    if (normalized == "remove") {
        return ActionType::REMOVE;
    }
    if (normalized == "update") {
        return ActionType::UPDATE;
    }
    if (normalized == "list") {
        return ActionType::LIST;
    }
    if (normalized == "search") {
        return ActionType::SEARCH;
    }
    if (normalized == "info") {
        return ActionType::INFO;
    }
    if (normalized == "outdated") {
        return ActionType::OUTDATED;
    }
    return ActionType::UNKNOWN;
}

struct FakeExecResponse {
    std::string match;
    ExecResult result;
};

struct PluginTestFixtureFile {
    std::string path;
    std::string content;
};

struct PluginTestCase {
    std::string name;
    std::string sourcePath;
    std::string actionName;
    std::string system;
    std::vector<std::string> flags{};
    std::vector<Package> packages{};
    std::string localPath;
    std::string prompt;
    std::filesystem::path fixtureRoot;
    std::vector<std::string> fixtureDirs{};
    std::vector<PluginTestFixtureFile> fixtureFiles{};
    std::map<std::string, std::string> environment{};
    std::vector<FakeExecResponse> fakeExec{};
    std::optional<bool> expectSuccess;
    std::vector<std::string> expectStdout{};
    std::vector<std::string> expectStderr{};
    std::vector<std::string> expectArtifacts{};
    std::vector<std::string> expectCommands{};
    std::vector<std::string> expectEvents{};
    std::map<std::string, std::string> expectEventPayloads{};
    std::optional<int> expectResultCount;
    std::optional<std::string> expectResultName;
    std::optional<std::string> expectResultVersion;
};

struct PluginTestRuntimeCaseResult {
    PluginTestCaseSummary summary;
    std::vector<PluginEventRecord> events{};
};

class PluginTestError : public std::runtime_error {
public:
    explicit PluginTestError(const std::string& message)
        : std::runtime_error(message) {}
};

std::filesystem::path default_fixture_root_for_case(const std::filesystem::path& casePath) {
    const std::string stem = casePath.stem().string();
    std::string sanitized;
    sanitized.reserve(stem.size());
    for (unsigned char ch : stem) {
        if (std::isalnum(ch) || ch == '-' || ch == '_') {
            sanitized.push_back(static_cast<char>(ch));
        } else {
            sanitized.push_back('-');
        }
    }
    if (sanitized.empty()) {
        sanitized = "case";
    }

    const std::size_t pathHash = std::hash<std::string>{}(casePath.string());
    std::ostringstream suffix;
    suffix << std::hex << pathHash;
    return (std::filesystem::temp_directory_path() / ("reqpack-plugin-test-" + sanitized + "-" + suffix.str())).lexically_normal();
}

std::map<std::string, std::string> string_map_from_table(const sol::object& object) {
    if (!object.valid() || object.is<sol::lua_nil_t>() || object.get_type() != sol::type::table) {
        return {};
    }

    std::map<std::string, std::string> values;
    for (const auto& [key, entry] : object.as<sol::table>()) {
        if (key.get_type() != sol::type::string || entry.get_type() != sol::type::string) {
            continue;
        }
        values.emplace(key.as<std::string>(), entry.as<std::string>());
    }
    return values;
}

std::vector<PluginTestFixtureFile> fixture_files_from_table(const sol::object& object, const std::filesystem::path& path) {
    if (!object.valid() || object.is<sol::lua_nil_t>() || object.get_type() != sol::type::table) {
        return {};
    }

    std::vector<PluginTestFixtureFile> files;
    for (const auto& [_, entry] : object.as<sol::table>()) {
        if (entry.get_type() != sol::type::table) {
            continue;
        }
        const sol::table file = entry.as<sol::table>();
        const sol::optional<std::string> fixturePath = file["path"];
        if (!fixturePath.has_value() || fixturePath->empty()) {
            throw PluginTestError("fixtureFiles.path is required: " + path.string());
        }
        const sol::optional<std::string> content = file["content"];
        files.push_back(PluginTestFixtureFile{
            .path = fixturePath.value(),
            .content = content.has_value() ? content.value() : std::string{},
        });
    }
    return files;
}

std::filesystem::path resolve_fixture_path(const PluginTestCase& testCase, const std::string& rawPath) {
    const std::filesystem::path path(rawPath);
    if (path.is_absolute()) {
        return path.lexically_normal();
    }
    return (testCase.fixtureRoot / path).lexically_normal();
}

std::string replace_all(std::string value, const std::string& needle, const std::string& replacement) {
    if (needle.empty()) {
        return value;
    }
    std::size_t position = 0;
    while ((position = value.find(needle, position)) != std::string::npos) {
        value.replace(position, needle.size(), replacement);
        position += replacement.size();
    }
    return value;
}

std::map<std::string, std::string> resolved_case_environment(const PluginTestCase& testCase) {
    std::map<std::string, std::string> values = testCase.environment;
    const std::string fixtureRoot = testCase.fixtureRoot.string();
    const std::string caseDirectory = std::filesystem::path(testCase.sourcePath).parent_path().string();
    for (auto& [_, value] : values) {
        value = replace_all(value, "${fixtureRoot}", fixtureRoot);
        value = replace_all(value, "${caseDir}", caseDirectory);
    }
    if (!fixtureRoot.empty() && values.find("REQPACK_TEST_FIXTURE_ROOT") == values.end()) {
        values.emplace("REQPACK_TEST_FIXTURE_ROOT", fixtureRoot);
    }
    return values;
}

void apply_case_placeholders(PluginTestCase& testCase) {
    const std::string fixtureRoot = testCase.fixtureRoot.string();
    const std::string caseDirectory = std::filesystem::path(testCase.sourcePath).parent_path().string();
    const auto expand = [&](const std::string& value) {
        return replace_all(replace_all(value, "${fixtureRoot}", fixtureRoot), "${caseDir}", caseDirectory);
    };

    testCase.localPath = expand(testCase.localPath);
    testCase.prompt = expand(testCase.prompt);
    for (FakeExecResponse& response : testCase.fakeExec) {
        response.match = expand(response.match);
        response.result.stdoutText = expand(response.result.stdoutText);
        response.result.stderrText = expand(response.result.stderrText);
    }
    for (std::string& value : testCase.expectCommands) {
        value = expand(value);
    }
    for (std::string& flag : testCase.flags) {
        flag = expand(flag);
    }
    for (Package& package : testCase.packages) {
        package.name = expand(package.name);
        package.version = expand(package.version);
        package.sourcePath = expand(package.sourcePath);
        for (std::string& flag : package.flags) {
            flag = expand(flag);
        }
    }
}

void write_fixture_file(const std::filesystem::path& path, const std::string& content) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        throw PluginTestError("failed to create fixture directory: " + path.parent_path().string());
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        throw PluginTestError("failed to write fixture file: " + path.string());
    }
    output << content;
}

void prepare_case_fixture_files(const PluginTestCase& testCase) {
    if (testCase.fixtureRoot.empty()) {
        return;
    }

    const std::string fixtureRoot = testCase.fixtureRoot.string();
    const std::string caseDirectory = std::filesystem::path(testCase.sourcePath).parent_path().string();

    std::error_code error;
    std::filesystem::remove_all(testCase.fixtureRoot, error);
    error.clear();
    std::filesystem::create_directories(testCase.fixtureRoot, error);
    if (error) {
        throw PluginTestError("failed to create fixture root: " + testCase.fixtureRoot.string());
    }

    for (const std::string& directory : testCase.fixtureDirs) {
        const std::filesystem::path resolved = resolve_fixture_path(testCase, directory);
        std::filesystem::create_directories(resolved, error);
        if (error) {
            throw PluginTestError("failed to create fixture directory: " + resolved.string());
        }
    }

    for (const PluginTestFixtureFile& file : testCase.fixtureFiles) {
        write_fixture_file(
            resolve_fixture_path(testCase, file.path),
            replace_all(replace_all(file.content, "${fixtureRoot}", fixtureRoot), "${caseDir}", caseDirectory)
        );
    }
}

class PreparedCaseFixtures {
public:
    explicit PreparedCaseFixtures(const std::vector<PluginTestCase>& cases) {
        roots_.reserve(cases.size());
        for (const PluginTestCase& testCase : cases) {
            prepare_case_fixture_files(testCase);
            if (!testCase.fixtureRoot.empty()) {
                roots_.push_back(testCase.fixtureRoot);
            }
        }
    }

    ~PreparedCaseFixtures() {
        for (const std::filesystem::path& root : roots_) {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    }

private:
    std::vector<std::filesystem::path> roots_{};
};

std::optional<std::string> environment_value(const std::string& name) {
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string{value};
}

void set_environment_value(const std::string& name, const std::optional<std::string>& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.has_value() ? value->c_str() : "");
#else
    if (value.has_value()) {
        setenv(name.c_str(), value->c_str(), 1);
    } else {
        unsetenv(name.c_str());
    }
#endif
}

class ScopedEnvironmentOverride {
public:
    explicit ScopedEnvironmentOverride(const std::map<std::string, std::string>& values) {
        previous_.reserve(values.size());
        for (const auto& [name, value] : values) {
            previous_.push_back({name, environment_value(name)});
            set_environment_value(name, value);
        }
    }

    ~ScopedEnvironmentOverride() {
        for (auto it = previous_.rbegin(); it != previous_.rend(); ++it) {
            set_environment_value(it->first, it->second);
        }
    }

private:
    std::vector<std::pair<std::string, std::optional<std::string>>> previous_{};
};

class FakeExecHost : public LuaBridge {
public:
    FakeExecHost(const std::string& scriptPath, const ReqPackConfig& config)
        : LuaBridge(scriptPath, config) {}

    void setCaseResponses(std::vector<FakeExecResponse> responses) {
        responses_ = std::move(responses);
        commands_.clear();
        stdout_.clear();
        stderr_.clear();
        artifacts_.clear();
        setExecOverride([this](const std::string& sourceId, const std::string& command) {
            return handleExecute(sourceId, command);
        });
    }

    const std::vector<std::string>& commands() const {
        return commands_;
    }

    const std::vector<std::string>& stdout_lines() const {
        return stdout_;
    }

    const std::vector<std::string>& stderr_lines() const {
        return stderr_;
    }

    const std::vector<std::string>& artifacts() const {
        return artifacts_;
    }

    void emitSuccess(const std::string& pluginId) override {
        recentEvents_.push_back(PluginEventRecord{.name = "success", .payload = "ok"});
        LuaBridge::emitSuccess(pluginId);
    }

    void emitFailure(const std::string& pluginId, const std::string& message) override {
        recentEvents_.push_back(PluginEventRecord{.name = "failed", .payload = message});
        LuaBridge::emitFailure(pluginId, message);
    }

    void emitEvent(const std::string& pluginId, const std::string& eventName, const std::string& payload) override {
        recentEvents_.push_back(PluginEventRecord{.name = eventName, .payload = payload});
        LuaBridge::emitEvent(pluginId, eventName, payload);
    }

    void registerArtifact(const std::string& pluginId, const std::string& payload) override {
        (void)pluginId;
        artifacts_.push_back(payload);
        LuaBridge::registerArtifact(pluginId, payload);
    }

    std::vector<PluginEventRecord> takeRecentEvents() override {
        std::vector<PluginEventRecord> events = std::move(recentEvents_);
        recentEvents_.clear();
        (void)LuaBridge::takeRecentEvents();
        return events;
    }

private:
    ExecResult handleExecute(const std::string& sourceId, const std::string& command) {
        (void)sourceId;
        commands_.push_back(command);
        for (const FakeExecResponse& response : responses_) {
            if (command.find(response.match) != std::string::npos) {
                if (!response.result.stdoutText.empty()) {
                    stdout_.push_back(response.result.stdoutText);
                }
                if (!response.result.stderrText.empty()) {
                    stderr_.push_back(response.result.stderrText);
                }
                return response.result;
            }
        }
        stderr_.push_back("no fakeExec rule matched command: " + command);
        return ExecResult{
            .success = false,
            .exitCode = 127,
            .stdoutText = {},
            .stderrText = "no fakeExec rule matched command: " + command,
        };
    }

    std::vector<FakeExecResponse> responses_{};
    std::vector<std::string> commands_{};
    std::vector<std::string> stdout_{};
    std::vector<std::string> stderr_{};
    std::vector<std::string> artifacts_{};
    std::vector<PluginEventRecord> recentEvents_{};
};

std::vector<std::filesystem::path> builtin_case_files_for_preset(const std::string& preset, const std::filesystem::path& pluginScript) {
    const std::string normalized = to_lower_copy(preset);
    if (normalized != "core") {
        throw PluginTestError("unknown plugin test preset: " + preset);
    }

    const std::filesystem::path presetDirectory = pluginScript.parent_path() / ".reqpack-test" / normalized;
    std::error_code error;
    if (!std::filesystem::exists(presetDirectory, error) || !std::filesystem::is_directory(presetDirectory, error)) {
        throw PluginTestError("preset '" + normalized + "' requires directory: " + presetDirectory.string());
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(presetDirectory, error)) {
        if (error) {
            throw PluginTestError("failed to read preset directory: " + presetDirectory.string());
        }
        if (entry.is_regular_file() && entry.path().extension() == ".lua") {
            files.push_back(std::filesystem::absolute(entry.path()).lexically_normal());
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        throw PluginTestError("preset '" + normalized + "' has no case files: " + presetDirectory.string());
    }
    return files;
}

std::vector<std::filesystem::path> collect_case_files(const PluginTestInvocation& invocation, const std::filesystem::path& pluginScript) {
    std::set<std::filesystem::path> uniquePaths;

    for (const std::string& preset : invocation.presets) {
        for (const std::filesystem::path& path : builtin_case_files_for_preset(preset, pluginScript)) {
            uniquePaths.insert(path);
        }
    }

    for (const std::string& raw : invocation.caseFiles) {
        uniquePaths.insert(std::filesystem::absolute(std::filesystem::path(raw)).lexically_normal());
    }

    for (const std::string& raw : invocation.caseDirectories) {
        const std::filesystem::path directory = std::filesystem::absolute(std::filesystem::path(raw)).lexically_normal();
        std::error_code error;
        if (!std::filesystem::exists(directory, error) || !std::filesystem::is_directory(directory, error)) {
            throw PluginTestError("case directory not found: " + directory.string());
        }

        for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
            if (error) {
                throw PluginTestError("failed to read case directory: " + directory.string());
            }
            if (!entry.is_regular_file()) {
                continue;
            }
            if (entry.path().extension() != ".lua") {
                continue;
            }
            uniquePaths.insert(std::filesystem::absolute(entry.path()).lexically_normal());
        }
    }

    std::vector<std::filesystem::path> paths(uniquePaths.begin(), uniquePaths.end());
    std::sort(paths.begin(), paths.end());
    return paths;
}

std::filesystem::path resolve_plugin_script(const ReqPackConfig& config, const std::string& pluginArgument) {
    const std::filesystem::path pluginPath(pluginArgument);
    std::error_code error;
    if (pluginPath.has_extension() && std::filesystem::is_regular_file(pluginPath, error) && !error) {
        return std::filesystem::absolute(pluginPath).lexically_normal();
    }

    error.clear();
    if (std::filesystem::is_directory(pluginPath, error) && !error) {
        const std::filesystem::path scriptPath = pluginPath / "run.lua";
        if (std::filesystem::is_regular_file(scriptPath, error) && !error) {
            return std::filesystem::absolute(scriptPath).lexically_normal();
        }
        throw PluginTestError("plugin directory does not contain run.lua: " + scriptPath.string());
    }

    const std::filesystem::path byId = std::filesystem::path(config.registry.pluginDirectory) / pluginArgument / "run.lua";
    error.clear();
    if (std::filesystem::is_regular_file(byId, error) && !error) {
        return std::filesystem::absolute(byId).lexically_normal();
    }

    throw PluginTestError("plugin not found: " + pluginArgument);
}

Package package_from_case_table(const sol::table& table, const std::string& fallbackSystem, const std::string& fallbackAction) {
    Package package;
    package.action = action_from_string(optional_string_field(table, "action").value_or(fallbackAction));
    package.system = optional_string_field(table, "system").value_or(fallbackSystem);
    package.name = optional_string_field(table, "name").value_or(std::string{});
    package.version = optional_string_field(table, "version").value_or(std::string{});
    package.sourcePath = optional_string_field(table, "sourcePath").value_or(std::string{});
    if (const sol::optional<bool> localTarget = table["localTarget"]; localTarget.has_value()) {
        package.localTarget = localTarget.value();
    }
    package.flags = string_list_from_table(table["flags"]);
    return package;
}

PluginTestCase load_case_file(const std::filesystem::path& path) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::math);

    sol::load_result loadResult = lua.load_file(path.string());
    if (!loadResult.valid()) {
        sol::error error = loadResult;
        throw PluginTestError("failed to load case file '" + path.string() + "': " + error.what());
    }

    sol::protected_function function = loadResult;
    sol::protected_function_result result = function();
    if (!result.valid()) {
        sol::error error = result;
        throw PluginTestError("failed to execute case file '" + path.string() + "': " + error.what());
    }
    if (result.return_count() == 0 || !result.get<sol::object>().is<sol::table>()) {
        throw PluginTestError("case file must return a table: " + path.string());
    }

    const sol::table root = result.get<sol::table>();
    PluginTestCase testCase;
    testCase.sourcePath = path.string();
    testCase.name = optional_string_field(root, "name").value_or(path.stem().string());
    testCase.fixtureRoot = default_fixture_root_for_case(path);

    const sol::object requestObject = root["request"];
    if (!requestObject.valid() || requestObject.get_type() != sol::type::table) {
        throw PluginTestError("case missing request table: " + path.string());
    }
    const sol::table request = requestObject.as<sol::table>();
    testCase.actionName = optional_string_field(request, "action").value_or(std::string{});
    if (testCase.actionName.empty()) {
        throw PluginTestError("case request.action is required: " + path.string());
    }
    testCase.system = optional_string_field(request, "system").value_or(std::string{});
    testCase.flags = string_list_from_table(request["flags"]);
    testCase.localPath = optional_string_field(request, "localPath").value_or(std::string{});
    testCase.prompt = optional_string_field(request, "prompt").value_or(std::string{});

    const sol::object packagesObject = request["packages"];
    if (packagesObject.valid() && packagesObject.get_type() == sol::type::table) {
        for (const auto& [_, entry] : packagesObject.as<sol::table>()) {
            if (entry.get_type() != sol::type::table) {
                continue;
            }
            testCase.packages.push_back(package_from_case_table(entry.as<sol::table>(), testCase.system, testCase.actionName));
        }
    }

    const sol::object fakeExecObject = root["fakeExec"];
    if (fakeExecObject.valid() && fakeExecObject.get_type() == sol::type::table) {
        for (const auto& [_, entry] : fakeExecObject.as<sol::table>()) {
            if (entry.get_type() != sol::type::table) {
                continue;
            }
            const sol::table fakeExec = entry.as<sol::table>();
            FakeExecResponse response;
            response.match = optional_string_field(fakeExec, "match").value_or(std::string{});
            if (response.match.empty()) {
                throw PluginTestError("fakeExec.match is required: " + path.string());
            }
            if (const sol::optional<bool> success = fakeExec["success"]; success.has_value()) {
                response.result.success = success.value();
            }
            if (const sol::optional<int> exitCode = fakeExec["exitCode"]; exitCode.has_value()) {
                response.result.exitCode = exitCode.value();
            }
            response.result.stdoutText = optional_string_field(fakeExec, "stdout").value_or(std::string{});
            response.result.stderrText = optional_string_field(fakeExec, "stderr").value_or(std::string{});
            if (!fakeExec["success"].valid()) {
                response.result.success = response.result.exitCode == 0;
            }
            testCase.fakeExec.push_back(std::move(response));
        }
    }

    if (const sol::optional<std::string> fixtureRoot = root["fixtureRoot"]; fixtureRoot.has_value() && !fixtureRoot->empty()) {
        const std::filesystem::path customRoot(fixtureRoot.value());
        testCase.fixtureRoot = customRoot.is_absolute()
            ? customRoot.lexically_normal()
            : (path.parent_path() / customRoot).lexically_normal();
    }
    testCase.fixtureDirs = string_list_from_table(root["fixtureDirs"]);
    testCase.fixtureFiles = fixture_files_from_table(root["fixtureFiles"], path);
    testCase.environment = string_map_from_table(root["environment"]);

    const sol::object expectObject = root["expect"];
    if (expectObject.valid() && expectObject.get_type() == sol::type::table) {
        const sol::table expect = expectObject.as<sol::table>();
        if (const sol::optional<bool> success = expect["success"]; success.has_value()) {
            testCase.expectSuccess = success.value();
        }
        testCase.expectStdout = string_list_from_table(expect["stdout"]);
        testCase.expectStderr = string_list_from_table(expect["stderr"]);
        testCase.expectArtifacts = string_list_from_table(expect["artifacts"]);
        if (const sol::optional<int> count = expect["resultCount"]; count.has_value()) {
            testCase.expectResultCount = count.value();
        }
        testCase.expectResultName = optional_string_field(expect, "resultName");
        testCase.expectResultVersion = optional_string_field(expect, "resultVersion");
        testCase.expectCommands = string_list_from_table(expect["commands"]);
        testCase.expectEvents = string_list_from_table(expect["events"]);
        const sol::object payloadsObject = expect["eventPayloads"];
        if (payloadsObject.valid() && payloadsObject.get_type() == sol::type::table) {
            for (const auto& [key, value] : payloadsObject.as<sol::table>()) {
                if (key.get_type() != sol::type::string || value.get_type() != sol::type::string) {
                    continue;
                }
                testCase.expectEventPayloads.emplace(key.as<std::string>(), value.as<std::string>());
            }
        }
    }

    apply_case_placeholders(testCase);

    return testCase;
}

PluginCallContext make_test_context(LuaBridge& bridge, const ReqPackConfig& config, const PluginTestCase& testCase) {
    return PluginCallContext{
        .pluginId = bridge.getPluginId(),
        .pluginDirectory = bridge.getPluginDirectory(),
        .scriptPath = bridge.getScriptPath(),
        .flags = testCase.flags,
        .host = bridge.getRuntimeHost(),
        .proxy = proxy_config_for_system(config, bridge.getPluginId()),
        .currentItemId = bridge.getPluginId(),
        .repositories = repositories_for_ecosystem(config, bridge.getPluginId()),
        .hostInfo = HostInfoService::currentSnapshot(),
    };
}

std::string package_display_name(const PackageInfo& info) {
    if (!info.name.empty()) {
        return info.name;
    }
    return info.packageId;
}

PluginTestRuntimeCaseResult run_single_case(FakeExecHost& plugin, const ReqPackConfig& config, const PluginTestCase& testCase) {
    plugin.setCaseResponses(testCase.fakeExec);
    ScopedEnvironmentOverride environmentOverride(resolved_case_environment(testCase));

    PluginTestRuntimeCaseResult result;
    result.summary.name = testCase.name;

    const PluginCallContext context = make_test_context(plugin, config, testCase);
    const ActionType action = action_from_string(testCase.actionName);
    if (action == ActionType::UNKNOWN) {
        throw PluginTestError("unsupported test action in case '" + testCase.name + "': " + testCase.actionName);
    }

    bool callSuccess = false;
    std::vector<PackageInfo> listResult;
    PackageInfo infoResult;

    switch (action) {
        case ActionType::INSTALL:
            if (!testCase.localPath.empty()) {
                callSuccess = plugin.installLocal(context, testCase.localPath);
            } else {
                callSuccess = plugin.install(context, testCase.packages);
            }
            break;
        case ActionType::REMOVE:
            callSuccess = plugin.remove(context, testCase.packages);
            break;
        case ActionType::UPDATE:
            callSuccess = plugin.update(context, testCase.packages);
            break;
        case ActionType::LIST:
            listResult = plugin.list(context);
            callSuccess = true;
            break;
        case ActionType::SEARCH:
            listResult = plugin.search(context, testCase.prompt);
            callSuccess = true;
            break;
        case ActionType::OUTDATED:
            listResult = plugin.outdated(context);
            callSuccess = true;
            break;
        case ActionType::INFO:
            infoResult = plugin.info(context, testCase.prompt);
            callSuccess = !package_display_name(infoResult).empty() || !infoResult.version.empty() || !infoResult.description.empty();
            break;
        default:
            throw PluginTestError("unsupported test action in case '" + testCase.name + "': " + testCase.actionName);
    }

    result.events = plugin.takeRecentEvents();
    result.summary.commands = plugin.commands();
    result.summary.stdoutLines = plugin.stdout_lines();
    result.summary.stderrLines = plugin.stderr_lines();
    result.summary.artifacts = plugin.artifacts();
    for (const PluginEventRecord& event : result.events) {
        result.summary.events.push_back(event.name);
        result.summary.eventRecords.push_back(PluginTestCaseSummary::EventRecord{.name = event.name, .payload = event.payload});
    }

    std::vector<std::string> failures;

    if (testCase.expectSuccess.has_value() && callSuccess != testCase.expectSuccess.value()) {
        failures.push_back("expected success=" + std::string(testCase.expectSuccess.value() ? "true" : "false"));
    }

    if (!testCase.expectCommands.empty()) {
        if (result.summary.commands != testCase.expectCommands) {
            failures.push_back("commands did not match expectation");
        }
    }

    if (!testCase.expectStdout.empty()) {
        if (result.summary.stdoutLines != testCase.expectStdout) {
            failures.push_back("stdout did not match expectation");
        }
    }

    if (!testCase.expectStderr.empty()) {
        if (result.summary.stderrLines != testCase.expectStderr) {
            failures.push_back("stderr did not match expectation");
        }
    }

    if (!testCase.expectArtifacts.empty()) {
        if (result.summary.artifacts != testCase.expectArtifacts) {
            failures.push_back("artifacts did not match expectation");
        }
    }

    if (!testCase.expectEvents.empty()) {
        if (result.summary.events != testCase.expectEvents) {
            failures.push_back("events did not match expectation");
        }
    }

    for (const auto& [name, payload] : testCase.expectEventPayloads) {
        const auto eventIt = std::find_if(result.summary.eventRecords.begin(), result.summary.eventRecords.end(), [&](const PluginTestCaseSummary::EventRecord& event) {
            return event.name == name;
        });
        if (eventIt == result.summary.eventRecords.end() || eventIt->payload != payload) {
            failures.push_back("event payload mismatch for " + name);
        }
    }

    if (testCase.expectResultCount.has_value()) {
        const int actualCount = action == ActionType::INFO ? (callSuccess ? 1 : 0) : static_cast<int>(listResult.size());
        if (actualCount != testCase.expectResultCount.value()) {
            failures.push_back("resultCount mismatch");
        }
    }

    if (testCase.expectResultName.has_value()) {
        const std::string actualName = action == ActionType::INFO
            ? package_display_name(infoResult)
            : (!listResult.empty() ? package_display_name(listResult.front()) : std::string{});
        if (actualName != testCase.expectResultName.value()) {
            failures.push_back("resultName mismatch");
        }
    }

    if (testCase.expectResultVersion.has_value()) {
        const std::string actualVersion = action == ActionType::INFO
            ? infoResult.version
            : (!listResult.empty() ? listResult.front().version : std::string{});
        if (actualVersion != testCase.expectResultVersion.value()) {
            failures.push_back("resultVersion mismatch");
        }
    }

    result.summary.success = failures.empty();
    if (!result.summary.success) {
        std::ostringstream message;
        for (std::size_t index = 0; index < failures.size(); ++index) {
            if (index != 0) {
                message << "; ";
            }
            message << failures[index];
        }
        result.summary.message = message.str();
    }

    return result;
}

void write_report_if_requested(const PluginTestInvocation& invocation, const PluginTestRunReport& report) {
    if (!invocation.reportPath.has_value()) {
        return;
    }
    const std::filesystem::path reportPath = std::filesystem::absolute(std::filesystem::path(invocation.reportPath.value())).lexically_normal();
    std::filesystem::create_directories(reportPath.parent_path());
    std::ofstream output(reportPath, std::ios::binary);
    if (!output.is_open()) {
        throw PluginTestError("failed to write report: " + reportPath.string());
    }
    output << plugin_test_report_to_json(report);
}

}  // namespace

namespace plugin_test_runner_internal {

PluginTestRunReport run_plugin_test_cases_impl(const ReqPackConfig& config, const PluginTestInvocation& invocation) {
    PluginTestRunReport report;

    const std::filesystem::path pluginScript = resolve_plugin_script(config, invocation.plugin);
    const std::vector<std::filesystem::path> caseFiles = collect_case_files(invocation, pluginScript);
    if (caseFiles.empty()) {
        throw PluginTestError("no case files found");
    }

    std::vector<PluginTestCase> loadedCases;
    loadedCases.reserve(caseFiles.size());
    std::vector<FakeExecResponse> bootstrapResponses;
    for (const std::filesystem::path& caseFile : caseFiles) {
        PluginTestCase testCase = load_case_file(caseFile);
        bootstrapResponses.insert(bootstrapResponses.end(), testCase.fakeExec.begin(), testCase.fakeExec.end());
        loadedCases.push_back(std::move(testCase));
    }

    PreparedCaseFixtures preparedFixtures(loadedCases);

    FakeExecHost plugin(pluginScript.string(), config);
    plugin.setCaseResponses(std::move(bootstrapResponses));
    if (!plugin.init()) {
        throw PluginTestError("failed to initialize plugin: " + pluginScript.string());
    }
    (void)plugin.takeRecentEvents();

    report.pluginId = plugin.getPluginId();
    report.pluginPath = pluginScript.string();

    for (const PluginTestCase& testCase : loadedCases) {
        PluginTestRuntimeCaseResult caseResult;
        try {
            caseResult = run_single_case(plugin, config, testCase);
        } catch (const std::exception& error) {
            caseResult.summary.name = testCase.name.empty() ? std::filesystem::path(testCase.sourcePath).stem().string() : testCase.name;
            caseResult.summary.success = false;
            caseResult.summary.message = error.what();
        }

        if (caseResult.summary.success) {
            ++report.passed;
        } else {
            ++report.failed;
        }
        report.cases.push_back(std::move(caseResult.summary));
    }

    (void)plugin.shutdown();
    write_report_if_requested(invocation, report);
    return report;
}

}  // namespace plugin_test_runner_internal
