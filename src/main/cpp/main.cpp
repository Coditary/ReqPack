#include "cli/cli.h"
#include "core/config/configuration.h"
#include "core/download/downloader.h"
#include "core/host/host_info.h"
#include "core/execution/orchestrator.h"
#include "core/plugins/plugin_test_runner.h"
#include "core/remote/remote_client.h"
#include "core/remote/serve_remote.h"
#include "output/display_factory.h"
#include "output/diagnostic.h"
#include "output/logger.h"
#include "plugins/lua_bridge.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <curl/curl.h>

#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <spawn.h>
#include <sstream>
#include <string_view>
#include <sys/wait.h>

extern char** environ;

namespace {

constexpr const char* DEFAULT_MAIN_REGISTRY_URL = "https://github.com/Coditary/rqp-registry.git";
constexpr const char* DEFAULT_SELF_UPDATE_RELEASE_API_BASE_URL = "https://api.github.com";

using boost::property_tree::ptree;

void configure_logger_from_config(Logger& logger, const ReqPackConfig& config) {
    logger.setLevel(to_string(config.logging.level));
    logger.setPattern(config.logging.pattern);
    logger.setBacktrace(config.logging.enableBacktrace, config.logging.backtraceSize);
    logger.setConsoleOutput(config.logging.consoleOutput);
    logger.setEnabledCategories(config.logging.enabledCategories);
    logger.setCaptureDisplayEvents(config.logging.captureDisplayEvents);
    if (config.logging.fileOutput) {
        logger.setFileSink(config.logging.filePath);
    } else {
        logger.disableFileSink();
    }
    if (config.logging.structuredFileOutput) {
        logger.setStructuredFileSink(config.logging.structuredFilePath);
    } else {
        logger.disableStructuredFileSink();
    }
}

DiagnosticMessage config_override_diagnostic(const std::string& details) {
    return make_error_diagnostic(
        "config",
        "Invalid logging or runtime option",
        "One or more CLI configuration flags could not be parsed.",
        "Check flag spelling and required values, then run command again.",
        details,
        "cli",
        "config"
    );
}

DiagnosticMessage stdin_syntax_diagnostic(std::size_t lineNumber, const std::string& commandText = {}) {
    return make_error_diagnostic(
        "cli",
        "stdin line " + std::to_string(lineNumber) + ": invalid command syntax",
        "Input line could not be tokenized because quotes or escapes are incomplete.",
        "Fix shell-style quoting on that line and try again.",
        commandText,
        "stdin",
        "batch",
        {{"line", std::to_string(lineNumber)}}
    );
}

DiagnosticMessage stdin_parse_diagnostic(std::size_t lineNumber, const std::string& commandText) {
    return make_error_diagnostic(
        "cli",
        "stdin line " + std::to_string(lineNumber) + ": command could not be parsed",
        "Parsed tokens did not form a valid rqp command.",
        "Check command structure or run same command directly with --help.",
        commandText,
        "stdin",
        "batch",
        {{"line", std::to_string(lineNumber)}}
    );
}

DiagnosticMessage stdin_install_only_diagnostic(std::size_t lineNumber) {
    return make_error_diagnostic(
        "cli",
        "stdin line " + std::to_string(lineNumber) + ": only install commands are allowed here",
        "`rqp install --stdin` accepts only install subcommands.",
        "Use `rqp serve --stdin` for mixed commands, or keep stdin input to install commands only.",
        {},
        "stdin",
        "batch",
        {{"line", std::to_string(lineNumber)}}
    );
}

DiagnosticMessage stdin_empty_batch_diagnostic() {
    return make_error_diagnostic(
        "cli",
        "stdin contained no install commands",
        "Only comments or empty lines were provided to install batch mode.",
        "Pipe at least one `install ...` command into rqp or use normal CLI arguments.",
        {},
        "stdin",
        "batch"
    );
}

DiagnosticMessage self_update_diagnostic(const std::string& summary, const std::string& cause, const std::string& recommendation, const std::string& details = {}) {
    return make_error_diagnostic(
        "self-update",
        summary,
        cause,
        recommendation,
        details,
        "self-update",
        "update"
    );
}

struct StdinCommand {
    std::size_t lineNumber{0};
    std::string text;
};

struct RemoteClientInvocation {
    std::string profileName;
    std::vector<std::string> forwardedArguments;
};

std::string trim_copy(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::optional<std::string> trim_line(const std::string& value) {
    const std::string trimmed = trim_copy(value);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    return trimmed;
}

std::vector<StdinCommand> read_stdin_commands(std::istream& input) {
    std::vector<StdinCommand> commands;
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        const std::string trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed.rfind("#", 0) == 0) {
            continue;
        }
        commands.push_back(StdinCommand{.lineNumber = lineNumber, .text = trimmed});
    }
    return commands;
}

std::vector<std::string> tokenize_command_line(const std::string& command) {
    std::vector<std::string> tokens;
    std::string current;
    bool inSingleQuotes = false;
    bool inDoubleQuotes = false;
    bool escaping = false;

    for (char c : command) {
        if (escaping) {
            current.push_back(c);
            escaping = false;
            continue;
        }

        if (c == '\\' && !inSingleQuotes) {
            escaping = true;
            continue;
        }

        if (c == '\'' && !inDoubleQuotes) {
            inSingleQuotes = !inSingleQuotes;
            continue;
        }

        if (c == '"' && !inSingleQuotes) {
            inDoubleQuotes = !inDoubleQuotes;
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(c)) && !inSingleQuotes && !inDoubleQuotes) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }

        current.push_back(c);
    }

    if (escaping || inSingleQuotes || inDoubleQuotes) {
        return {};
    }

    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }

    return tokens;
}

bool contains_flag(const std::vector<std::string>& arguments, const std::string& flag) {
    for (const std::string& argument : arguments) {
        if (argument == flag) {
            return true;
        }
    }
    return false;
}

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool is_existing_path(const std::string& value) {
    std::error_code error;
    return std::filesystem::exists(std::filesystem::path(value), error) && !error;
}

bool is_url(const std::string& value) {
    return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0 || value.rfind("file://", 0) == 0;
}

std::vector<std::string> strip_config_arguments(const std::vector<std::string>& arguments) {
    std::vector<std::string> filtered;
    filtered.reserve(arguments.size());
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        ReqPackConfigOverrides ignoredOverrides;
        std::size_t configIndex = i;
        if (consume_cli_config_flag(arguments, configIndex, ignoredOverrides)) {
            i = configIndex;
            continue;
        }
        filtered.push_back(arguments[i]);
    }
    return filtered;
}

bool is_install_stdin_command(const std::vector<std::string>& arguments) {
    const std::vector<std::string> filtered = strip_config_arguments(arguments);
    return filtered.size() >= 2 && filtered.front() == "install" && contains_flag(filtered, "--stdin");
}

std::vector<std::string> inherited_stream_arguments(const std::vector<std::string>& arguments) {
    const std::vector<std::string> filtered = strip_config_arguments(arguments);
    std::vector<std::string> inherited;
    inherited.reserve(filtered.size());
    bool actionSeen = false;

    for (const std::string& argument : filtered) {
        if (!actionSeen) {
            if (argument == "install" || argument == "serve") {
                actionSeen = true;
            }
            continue;
        }
        if (argument == "--stdin") {
            continue;
        }
        inherited.push_back(argument);
    }

    return inherited;
}

bool parse_int_flag_value(
    const std::vector<std::string>& arguments,
    std::size_t& index,
    const std::string& argument,
    const std::string& flag,
    int& target,
    std::string& error
) {
    std::string value;
    if (argument == flag) {
        if (index + 1 >= arguments.size()) {
            error = flag + " requires a value";
            return false;
        }
        value = arguments[++index];
    } else if (starts_with(argument, flag + "=")) {
        value = argument.substr(flag.size() + 1);
    } else {
        return true;
    }

    try {
        target = std::stoi(value);
    } catch (...) {
        error = "invalid numeric value for " + flag + ": " + value;
        return false;
    }
    return true;
}

bool parse_string_flag_value(
    const std::vector<std::string>& arguments,
    std::size_t& index,
    const std::string& argument,
    const std::string& flag,
    std::string& target,
    std::string& error
) {
    if (argument == flag) {
        if (index + 1 >= arguments.size()) {
            error = flag + " requires a value";
            return false;
        }
        target = arguments[++index];
        return true;
    }
    if (starts_with(argument, flag + "=")) {
        target = argument.substr(flag.size() + 1);
    }
    return true;
}

bool parse_optional_string_flag_value(
    const std::vector<std::string>& arguments,
    std::size_t& index,
    const std::string& argument,
    const std::string& flag,
    std::optional<std::string>& target,
    std::string& error
) {
    std::string value;
    if (argument == flag) {
        if (index + 1 >= arguments.size()) {
            error = flag + " requires a value";
            return false;
        }
        value = arguments[++index];
    } else if (starts_with(argument, flag + "=")) {
        value = argument.substr(flag.size() + 1);
    } else {
        return true;
    }
    target = value;
    return true;
}

bool parse_serve_runtime_options(
    const std::vector<std::string>& arguments,
    ServeRuntimeOptions& options,
    std::string& error
) {
    const std::vector<std::string> filtered = strip_config_arguments(arguments);
    if (filtered.empty() || filtered.front() != "serve") {
        return false;
    }

    options = ServeRuntimeOptions{};

    for (std::size_t i = 1; i < filtered.size(); ++i) {
        const std::string& argument = filtered[i];
        if (argument == "--stdin") {
            options.stdin = true;
            continue;
        }
        if (argument == "--remote") {
            options.remote = true;
            continue;
        }
        if (argument == "--json") {
            options.remoteProtocol = ServeRemoteProtocol::JSON;
            continue;
        }
        if (argument == "--http") {
            options.remoteProtocol = ServeRemoteProtocol::HTTP;
            continue;
        }
        if (argument == "--https") {
            options.remoteProtocol = ServeRemoteProtocol::HTTPS;
            continue;
        }
        if (argument == "--readonly") {
            options.readonly = true;
            options.readonlyExplicit = true;
            continue;
        }
        if (argument == "--bind" || starts_with(argument, "--bind=")) {
            if (!parse_string_flag_value(filtered, i, argument, "--bind", options.bind, error)) {
                return true;
            }
            continue;
        }
        if (argument == "--port" || starts_with(argument, "--port=")) {
            if (!parse_int_flag_value(filtered, i, argument, "--port", options.port, error)) {
                return true;
            }
            continue;
        }
        if (argument == "--max-connections" || starts_with(argument, "--max-connections=")) {
            if (!parse_int_flag_value(filtered, i, argument, "--max-connections", options.maxConnections, error)) {
                return true;
            }
            options.maxConnectionsExplicit = true;
            continue;
        }
        if (argument == "--token" || starts_with(argument, "--token=")) {
            if (!parse_optional_string_flag_value(filtered, i, argument, "--token", options.token, error)) {
                return true;
            }
            continue;
        }
        if (argument == "--username" || starts_with(argument, "--username=")) {
            if (!parse_optional_string_flag_value(filtered, i, argument, "--username", options.username, error)) {
                return true;
            }
            continue;
        }
        if (argument == "--password" || starts_with(argument, "--password=")) {
            if (!parse_optional_string_flag_value(filtered, i, argument, "--password", options.password, error)) {
                return true;
            }
            continue;
        }

        options.inheritedArguments.push_back(argument);
    }

    if (options.stdin == options.remote) {
        error = "serve requires exactly one of --stdin or --remote";
        return true;
    }
    if (options.port <= 0 || options.port > 65535) {
        error = "--port must be between 1 and 65535";
        return true;
    }
    if (options.maxConnections <= 0) {
        error = "--max-connections must be greater than 0";
        return true;
    }
    if (options.stdin) {
        if (options.token.has_value() || options.username.has_value() || options.password.has_value() ||
            options.bind != "127.0.0.1" || options.port != 4545 || options.remoteProtocol != ServeRemoteProtocol::TEXT ||
            options.maxConnections != 16) {
            error = "remote-only flags require --remote";
            return true;
        }
    }
    if (options.token.has_value() && (options.username.has_value() || options.password.has_value())) {
        error = "use either --token or --username/--password, not both";
        return true;
    }
    if (options.username.has_value() != options.password.has_value()) {
        error = "--username and --password must be used together";
        return true;
    }
    if (!options.remote && options.remoteProtocol != ServeRemoteProtocol::TEXT) {
        error = "protocol flags require --remote";
        return true;
    }

    return true;
}

bool parse_remote_client_invocation(
    const std::vector<std::string>& arguments,
    RemoteClientInvocation& invocation,
    std::string& error
) {
    invocation = RemoteClientInvocation{};

    std::size_t actionIndex = arguments.size();
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        ReqPackConfigOverrides ignoredOverrides;
        std::size_t configIndex = i;
        if (consume_cli_config_flag(arguments, configIndex, ignoredOverrides)) {
            i = configIndex;
            continue;
        }
        actionIndex = i;
        break;
    }

    if (actionIndex >= arguments.size() || arguments[actionIndex] != "remote") {
        return false;
    }
    if (actionIndex + 1 >= arguments.size()) {
        error = "remote requires a profile name";
        return true;
    }

    invocation.profileName = arguments[actionIndex + 1];
    invocation.forwardedArguments.assign(arguments.begin() + static_cast<std::ptrdiff_t>(actionIndex + 2), arguments.end());
    return true;
}

std::vector<std::string> merged_stream_command_arguments(
    const std::vector<std::string>& commandTokens,
    const std::vector<std::string>& inheritedArguments
) {
    std::vector<std::string> merged;
    merged.reserve(commandTokens.size() + inheritedArguments.size());
    merged.insert(merged.end(), commandTokens.begin(), commandTokens.end());
    merged.insert(merged.end(), inheritedArguments.begin(), inheritedArguments.end());
    return merged;
}

bool run_process(const std::vector<std::string>& arguments, const std::filesystem::path& workingDirectory) {
    if (arguments.empty()) {
        return false;
    }

    posix_spawn_file_actions_t fileActions;
    if (posix_spawn_file_actions_init(&fileActions) != 0) {
        return false;
    }

    bool ready = true;
#if defined(__linux__) || defined(__APPLE__)
    if (!workingDirectory.empty()) {
        ready = posix_spawn_file_actions_addchdir_np(&fileActions, workingDirectory.c_str()) == 0;
    }
#endif
    if (!ready) {
        posix_spawn_file_actions_destroy(&fileActions);
        return false;
    }

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const std::string& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int spawnResult = posix_spawnp(&pid, arguments.front().c_str(), &fileActions, nullptr, argv.data(), ::environ);
    posix_spawn_file_actions_destroy(&fileActions);
    if (spawnResult != 0) {
        return false;
    }

    int status = 0;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            return false;
        }
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool ensure_directory(const std::filesystem::path& path) {
    if (path.empty()) {
        return true;
    }
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return !error;
}

bool copy_file_with_mode(const std::filesystem::path& source, const std::filesystem::path& target) {
    std::error_code error;
    if (!ensure_directory(target.parent_path())) {
        return false;
    }
    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
        return false;
    }
    const auto permissions = std::filesystem::status(source, error).permissions();
    if (!error) {
        std::filesystem::permissions(target, permissions, std::filesystem::perm_options::replace, error);
    }
    return !error;
}

bool copy_directory_contents_with_mode(const std::filesystem::path& sourceRoot, const std::filesystem::path& targetRoot) {
    std::error_code error;
    if (!ensure_directory(targetRoot)) {
        return false;
    }

    for (auto it = std::filesystem::recursive_directory_iterator(sourceRoot, error);
         it != std::filesystem::recursive_directory_iterator();
         it.increment(error)) {
        if (error) {
            return false;
        }

        const std::filesystem::path relativePath = std::filesystem::relative(it->path(), sourceRoot, error);
        if (error) {
            return false;
        }
        const std::filesystem::path targetPath = targetRoot / relativePath;

        if (it->is_directory(error)) {
            if (error || !ensure_directory(targetPath)) {
                return false;
            }
            continue;
        }

        if (it->is_regular_file(error)) {
            if (error || !copy_file_with_mode(it->path(), targetPath)) {
                return false;
            }
            continue;
        }

        if (it->is_symlink(error)) {
            const std::filesystem::path linkTarget = std::filesystem::read_symlink(it->path(), error);
            if (error || !ensure_directory(targetPath.parent_path())) {
                return false;
            }
            std::filesystem::remove(targetPath, error);
            error.clear();
            std::filesystem::create_symlink(linkTarget, targetPath, error);
            if (error) {
                return false;
            }
            continue;
        }

        if (error) {
            return false;
        }
    }

    return true;
}

bool replace_symlink_atomically(const std::filesystem::path& target, const std::filesystem::path& linkPath) {
    std::error_code error;
    if (!ensure_directory(linkPath.parent_path())) {
        return false;
    }

    const std::filesystem::path tempLink = linkPath.parent_path() / (linkPath.filename().string() + ".tmp");
    std::filesystem::remove(tempLink, error);
    error.clear();
    std::filesystem::create_symlink(target, tempLink, error);
    if (error) {
        return false;
    }

    std::filesystem::rename(tempLink, linkPath, error);
    if (!error) {
        return true;
    }

    error.clear();
    std::filesystem::remove(linkPath, error);
    error.clear();
    std::filesystem::rename(tempLink, linkPath, error);
    return !error;
}

std::string sanitize_release_identifier(std::string value) {
    for (char& ch : value) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!(std::isalnum(c) || ch == '.' || ch == '-' || ch == '_')) {
            ch = '_';
        }
    }
    return value;
}

std::string normalized_self_update_release_api_base_url(const SelfUpdateConfig& config) {
    std::string value = trim_copy(config.releaseApiBaseUrl);
    if (value.empty()) {
        value = DEFAULT_SELF_UPDATE_RELEASE_API_BASE_URL;
    }
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

std::string release_tag_for_config(const SelfUpdateConfig& config) {
    const std::string configured = trim_copy(config.releaseTag);
    return configured.empty() ? std::string{"latest"} : configured;
}

std::optional<std::pair<std::string, std::string>> parse_release_owner_repo(const std::string& repoUrl) {
    const std::string trimmed = trim_copy(repoUrl);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    auto normalize_path = [](std::string path) {
        while (!path.empty() && path.front() == '/') {
            path.erase(path.begin());
        }
        while (!path.empty() && path.back() == '/') {
            path.pop_back();
        }
        if (path.size() > 4 && path.substr(path.size() - 4) == ".git") {
            path.erase(path.size() - 4);
        }
        return path;
    };

    std::string path = trimmed;
    if (const std::size_t schemePos = path.find("://"); schemePos != std::string::npos) {
        const std::size_t hostEnd = path.find('/', schemePos + 3);
        if (hostEnd == std::string::npos) {
            return std::nullopt;
        }
        path = path.substr(hostEnd + 1);
    } else if (const std::size_t colon = path.find(':'); colon != std::string::npos && path.find('@') != std::string::npos) {
        path = path.substr(colon + 1);
    }

    path = normalize_path(path);
    const std::size_t lastSlash = path.rfind('/');
    if (lastSlash == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t secondLastSlash = path.rfind('/', lastSlash - 1);
    const std::size_t ownerStart = secondLastSlash == std::string::npos ? 0 : secondLastSlash + 1;
    const std::string owner = path.substr(ownerStart, lastSlash - ownerStart);
    const std::string repo = path.substr(lastSlash + 1);
    if (!owner.empty() && !repo.empty()) {
        return std::make_pair(owner, repo);
    }

    return std::nullopt;
}

std::optional<std::string> self_update_release_target(const HostInfoSnapshot& snapshot) {
    if (!snapshot.platform.target.empty()) {
        if (snapshot.platform.target == "x86_64-linux" || snapshot.platform.target == "aarch64-linux" ||
            snapshot.platform.target == "x86_64-darwin" || snapshot.platform.target == "aarch64-darwin") {
            return snapshot.platform.target;
        }
    }
    return std::nullopt;
}

std::string self_update_release_api_url(const SelfUpdateConfig& config, const std::string& owner, const std::string& repo) {
    const std::string base = normalized_self_update_release_api_base_url(config);
    const std::string tag = release_tag_for_config(config);
    if (tag == "latest") {
        return base + "/repos/" + owner + "/" + repo + "/releases/latest";
    }
    return base + "/repos/" + owner + "/" + repo + "/releases/tags/" + tag;
}

std::optional<ptree> load_json_tree(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        return std::nullopt;
    }

    try {
        ptree tree;
        boost::property_tree::read_json(input, tree);
        return tree;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::pair<std::string, std::string>> resolve_self_update_asset(
    const SelfUpdateConfig& config,
    const std::string& owner,
    const std::string& repo,
    const std::string& releaseTarget,
    const std::filesystem::path& metadataPath
) {
    const std::optional<ptree> tree = load_json_tree(metadataPath);
    if (!tree.has_value()) {
        return std::nullopt;
    }

    const std::string tagName = trim_copy(tree->get<std::string>("tag_name", {}));
    if (tagName.empty()) {
        return std::nullopt;
    }

    const boost::optional<const ptree&> assets = tree->get_child_optional("assets");
    if (!assets) {
        return std::nullopt;
    }

    const std::string expectedAssetName = "rqp-" + tagName + "-" + releaseTarget + ".tar.gz";
    for (const auto& child : assets.get()) {
        const std::string assetName = trim_copy(child.second.get<std::string>("name", {}));
        if (assetName != expectedAssetName) {
            continue;
        }

        std::string assetUrl = trim_copy(child.second.get<std::string>("browser_download_url", {}));
        if (assetUrl.empty()) {
            assetUrl = "https://github.com/" + owner + "/" + repo + "/releases/download/" + tagName + "/" + assetName;
        }
        return std::make_pair(tagName, assetUrl);
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> create_self_update_temp_directory() {
    std::error_code error;
    const std::filesystem::path tempDirectory = std::filesystem::temp_directory_path(error);
    if (error) {
        return std::nullopt;
    }

    const std::filesystem::path base = tempDirectory / "reqpack-self-update";
    std::filesystem::create_directories(base, error);
    if (error) {
        return std::nullopt;
    }

    for (int attempt = 0; attempt < 100; ++attempt) {
        const std::filesystem::path candidate = base / ("session-" + std::to_string(::getpid()) + "-" + std::to_string(std::rand()));
        error.clear();
        if (std::filesystem::create_directory(candidate, error)) {
            return candidate;
        }
        if (error && error != std::errc::file_exists) {
            return std::nullopt;
        }
    }

    return std::nullopt;
}

bool remove_path_quietly(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    return !error;
}

bool extract_release_archive(const std::filesystem::path& archivePath, const std::filesystem::path& destinationPath) {
    if (!ensure_directory(destinationPath)) {
        return false;
    }
    return run_process({"tar", "-xzf", archivePath.string(), "-C", destinationPath.string()}, std::filesystem::current_path());
}

std::optional<std::filesystem::path> locate_extracted_binary(const std::filesystem::path& directory) {
    std::error_code error;
    std::optional<std::filesystem::path> bestMatch;
    std::size_t bestDepth = std::numeric_limits<std::size_t>::max();
    for (auto it = std::filesystem::recursive_directory_iterator(directory, error); it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (error) {
            return std::nullopt;
        }
        if (!it->is_regular_file()) {
            continue;
        }
        if (it->path().filename() == "rqp") {
            const std::filesystem::path relativePath = std::filesystem::relative(it->path(), directory, error);
            if (error) {
                return std::nullopt;
            }
            const std::size_t depth = static_cast<std::size_t>(std::distance(relativePath.begin(), relativePath.end()));
            if (!bestMatch.has_value() || depth < bestDepth) {
                bestMatch = it->path();
                bestDepth = depth;
            }
        }
    }
    return bestMatch;
}

std::optional<std::filesystem::path> current_link_target_path(const std::filesystem::path& linkPath) {
    std::error_code error;
    if (!std::filesystem::exists(linkPath, error) || error) {
        return std::nullopt;
    }
    if (!std::filesystem::is_symlink(linkPath, error) || error) {
        return std::nullopt;
    }

    const std::filesystem::path target = std::filesystem::read_symlink(linkPath, error);
    if (error || target.empty()) {
        return std::nullopt;
    }
    return target;
}

bool is_self_update_command(const std::vector<std::string>& arguments) {
    const std::vector<std::string> filtered = strip_config_arguments(arguments);
    if (filtered.empty() || filtered.front() != "update") {
        return false;
    }

    for (std::size_t i = 1; i < filtered.size(); ++i) {
        const std::string& argument = filtered[i];
        if (argument == "--help" || argument == "-h") {
            return false;
        }
        if (argument == "--all") {
            return false;
        }
        if (argument.rfind("--", 0) == 0) {
            continue;
        }
        return false;
    }
    return true;
}

bool is_host_refresh_command(const std::vector<std::string>& arguments) {
    const std::vector<std::string> filtered = strip_config_arguments(arguments);
    if (filtered.size() != 2) {
        return false;
    }
    return to_lower(filtered[0]) == "host" && to_lower(filtered[1]) == "refresh";
}

DiagnosticMessage plugin_test_diagnostic(const std::string& summary, const std::string& cause, const std::string& recommendation, const std::string& details = {}) {
    return make_error_diagnostic(
        "plugin-test",
        summary,
        cause,
        recommendation,
        details,
        "test-plugin",
        "conformance"
    );
}

int run_host_refresh(Logger& logger) {
    const std::shared_ptr<const HostInfoSnapshot> snapshot = HostInfoService::refreshSnapshot();
    logger.stdout("host refresh: cache updated");
    logger.stdout("host os: " + snapshot->platform.osFamily);
    logger.stdout("host arch: " + snapshot->platform.arch);
    logger.stdout("host cache: " + default_reqpack_host_info_cache_path().string());
    return 0;
}

int run_self_update(const ReqPackConfig& config, Logger& logger) {
    if (config.selfUpdate.repoUrl.empty()) {
        logger.diagnostic(self_update_diagnostic(
            "Self-update is not configured",
            "No repository URL is configured for self-update.",
            "Set selfUpdate.repoUrl in config before running `rqp update` without package arguments."
        ));
        return 1;
    }

    const std::optional<std::pair<std::string, std::string>> ownerRepo = parse_release_owner_repo(config.selfUpdate.repoUrl);
    if (!ownerRepo.has_value()) {
        logger.diagnostic(self_update_diagnostic(
            "Self-update repository is unsupported",
            "Configured self-update repository could not be mapped to a release owner and repository name.",
            "Use repository URL that ends with `/<owner>/<repo>.git` so release API path can be derived.",
            config.selfUpdate.repoUrl
        ));
        return 1;
    }

    if (config.selfUpdate.binaryDirectory.empty() || config.selfUpdate.linkPath.empty()) {
        logger.diagnostic(self_update_diagnostic(
            "Self-update paths are incomplete",
            "One or more required self-update paths are missing from configuration.",
            "Configure binaryDirectory and linkPath before running self-update."
        ));
        return 1;
    }

    const std::shared_ptr<const HostInfoSnapshot> snapshot = HostInfoService::currentSnapshot();
    const std::optional<std::string> releaseTarget = self_update_release_target(*snapshot);
    if (!releaseTarget.has_value()) {
        logger.diagnostic(self_update_diagnostic(
            "Self-update target is unsupported",
            "ReqPack does not have a matching release target for this host architecture.",
            "Use a supported release target or install ReqPack manually for this platform.",
            snapshot->platform.target.empty() ? (snapshot->platform.arch + "-" + snapshot->platform.osFamily) : snapshot->platform.target
        ));
        return 1;
    }

    const std::optional<std::filesystem::path> tempDirectory = create_self_update_temp_directory();
    if (!tempDirectory.has_value()) {
        logger.diagnostic(self_update_diagnostic(
            "Self-update workspace setup failed",
            "ReqPack could not create a temporary working directory for release download.",
            "Check temporary-directory permissions and free space, then retry self-update."
        ));
        return 1;
    }

    const std::filesystem::path tempPath = tempDirectory.value();
    const std::filesystem::path metadataPath = tempPath / "release.json";
    const std::filesystem::path archivePath = tempPath / "rqp.tar.gz";
    const std::filesystem::path extractPath = tempPath / "extract";
    const std::filesystem::path linkPath(config.selfUpdate.linkPath);
    const std::filesystem::path binaryDirectory(config.selfUpdate.binaryDirectory);
    const std::optional<std::filesystem::path> previousLinkTargetPath = current_link_target_path(linkPath);
    Downloader downloader(nullptr, config);

    const auto cleanup = [&tempPath]() {
        (void)remove_path_quietly(tempPath);
    };

    logger.stdout("self-update: fetch release metadata");
    if (!downloader.download(
            self_update_release_api_url(config.selfUpdate, ownerRepo->first, ownerRepo->second),
            metadataPath.string())) {
        cleanup();
        logger.diagnostic(self_update_diagnostic(
            "Self-update metadata download failed",
            "ReqPack could not read release metadata from configured release API.",
            "Check network access, releaseApiBaseUrl, repository visibility, and selected release tag."
        ));
        return 1;
    }

    const std::optional<std::pair<std::string, std::string>> asset = resolve_self_update_asset(
        config.selfUpdate,
        ownerRepo->first,
        ownerRepo->second,
        releaseTarget.value(),
        metadataPath
    );
    if (!asset.has_value()) {
        cleanup();
        logger.diagnostic(self_update_diagnostic(
            "Self-update release asset is missing",
            "Configured release does not contain a binary archive for this host target.",
            "Publish asset `rqp-<tag>-" + releaseTarget.value() + ".tar.gz` or choose a different release tag.",
            releaseTarget.value()
        ));
        return 1;
    }

    logger.stdout("self-update: download release archive");
    if (!downloader.download(asset->second, archivePath.string())) {
        cleanup();
        logger.diagnostic(self_update_diagnostic(
            "Self-update archive download failed",
            "ReqPack found matching release metadata, but binary archive download failed.",
            "Check release asset availability and network access, then retry self-update.",
            asset->second
        ));
        return 1;
    }

    logger.stdout("self-update: extract release archive");
    if (!extract_release_archive(archivePath, extractPath)) {
        cleanup();
        logger.diagnostic(self_update_diagnostic(
            "Self-update archive extraction failed",
            "Downloaded release archive could not be extracted on this system.",
            "Ensure `tar` is available and the release asset is a valid `.tar.gz` archive.",
            archivePath.string()
        ));
        return 1;
    }

    const std::optional<std::filesystem::path> extractedBinary = locate_extracted_binary(extractPath);
    if (!extractedBinary.has_value()) {
        cleanup();
        logger.diagnostic(self_update_diagnostic(
            "Self-update archive is invalid",
            "Release archive was extracted, but no `rqp` binary was found inside it.",
            "Republish release archive with `rqp` binary at archive root."
        ));
        return 1;
    }

    if (!ensure_directory(binaryDirectory)) {
        cleanup();
        logger.diagnostic(self_update_diagnostic(
            "Self-update install directory setup failed",
            "ReqPack could not create binary install directory for downloaded release.",
            "Check write permissions for selfUpdate.binaryDirectory.",
            binaryDirectory.string()
        ));
        return 1;
    }

    const std::filesystem::path installedBundlePath = binaryDirectory /
        ("rqp-" + sanitize_release_identifier(asset->first) + "-" + releaseTarget.value());
    std::error_code installCleanupError;
    std::filesystem::remove_all(installedBundlePath, installCleanupError);
    if (installCleanupError) {
        cleanup();
        logger.diagnostic(self_update_diagnostic(
            "Self-update install directory cleanup failed",
            "ReqPack could not prepare destination directory for downloaded release bundle.",
            "Check filesystem permissions for selfUpdate.binaryDirectory.",
            installedBundlePath.string()
        ));
        return 1;
    }

    if (!copy_directory_contents_with_mode(extractedBinary.value().parent_path(), installedBundlePath)) {
        cleanup();
        logger.diagnostic(self_update_diagnostic(
            "Self-update bundle install failed",
            "Downloaded ReqPack release bundle could not be copied into configured self-update binary directory.",
            "Check filesystem permissions for selfUpdate.binaryDirectory.",
            installedBundlePath.string()
        ));
        return 1;
    }

    const std::filesystem::path installedBinaryPath = installedBundlePath / "rqp";
    if (!std::filesystem::exists(installedBinaryPath)) {
        cleanup();
        logger.diagnostic(self_update_diagnostic(
            "Self-update bundle is invalid",
            "Downloaded release bundle was copied locally, but installed executable is missing.",
            "Republish release archive with `rqp` binary at archive root.",
            installedBundlePath.string()
        ));
        return 1;
    }

    logger.stdout("self-update: update local symlink");
    if (!replace_symlink_atomically(installedBinaryPath, linkPath)) {
        cleanup();
        logger.diagnostic(self_update_diagnostic(
            "Self-update symlink update failed",
            "New binary was downloaded, but ReqPack could not update configured executable symlink.",
            "Check write permissions for configured link path and parent directory.",
            config.selfUpdate.linkPath
        ));
        return 1;
    }

    cleanup();
    if (!HostInfoService::invalidateCache()) {
        logger.diagnostic(make_warning_diagnostic(
            "self-update",
            "Self-update completed, but host info cache could not be invalidated",
            "Old host metadata cache may remain until next refresh.",
            "Run `rqp host refresh` if plugins still report stale host information.",
            {},
            "self-update",
            "update"
        ));
    }

    if (previousLinkTargetPath.has_value() && previousLinkTargetPath.value() == installedBinaryPath) {
        logger.stdout("self-update complete: already on release " + asset->first);
    } else {
        logger.stdout("self-update complete: now on release " + asset->first);
    }
    logger.stdout("self-update link: " + config.selfUpdate.linkPath);
    return 0;
}

int run_stdin_install_batch(Cli& cli, const ReqPackConfig& config, const std::vector<std::string>& inheritedArguments) {
    Logger& logger = Logger::instance();
    const std::vector<StdinCommand> commands = read_stdin_commands(std::cin);
    std::vector<Request> requests;
    const ReqPackConfigOverrides inheritedOverrides = extract_cli_config_overrides(inheritedArguments);
    if (inheritedOverrides.errorMessage.has_value()) {
        logger.diagnostic(config_override_diagnostic(inheritedOverrides.errorMessage.value()));
        return 1;
    }
    ReqPackConfig effectiveConfig = apply_config_overrides(config, inheritedOverrides);

    for (const StdinCommand& command : commands) {
        const std::vector<std::string> commandTokens = tokenize_command_line(command.text);
        if (commandTokens.empty()) {
            logger.diagnostic(stdin_syntax_diagnostic(command.lineNumber, command.text));
            return 1;
        }

        const std::vector<std::string> mergedTokens = merged_stream_command_arguments(commandTokens, inheritedArguments);
        const ReqPackConfigOverrides mergedOverrides = extract_cli_config_overrides(mergedTokens);
        if (mergedOverrides.errorMessage.has_value()) {
            logger.diagnostic(config_override_diagnostic("stdin line " + std::to_string(command.lineNumber) + ": " + mergedOverrides.errorMessage.value()));
            return 1;
        }
        effectiveConfig = apply_config_overrides(effectiveConfig, mergedOverrides);
        const std::vector<Request> parsed = cli.parse(mergedTokens, effectiveConfig);
        if (parsed.empty()) {
            if (!cli.lastParseError().empty()) {
                logger.diagnostic(config_override_diagnostic("stdin line " + std::to_string(command.lineNumber) + ": " + cli.lastParseError()));
                return 1;
            }
            logger.diagnostic(stdin_parse_diagnostic(command.lineNumber, command.text));
            return 1;
        }

        for (const Request& request : parsed) {
            if (request.action != ActionType::INSTALL) {
                logger.diagnostic(stdin_install_only_diagnostic(command.lineNumber));
                return 1;
            }
            requests.push_back(request);
        }
    }

    if (requests.empty()) {
        logger.diagnostic(stdin_empty_batch_diagnostic());
        return 1;
    }

    Orchestrator orchestrator(std::move(requests), effectiveConfig);
    const int result = orchestrator.run();
    logger.flushSync();
    return result;
}

int run_stdin_serve_loop(Cli& cli, const ReqPackConfig& config, const std::vector<std::string>& inheritedArguments) {
    Logger& logger = Logger::instance();
    int exitCode = 0;

    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(std::cin, line)) {
        ++lineNumber;
        const std::string trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed.rfind("#", 0) == 0) {
            continue;
        }

        const std::vector<std::string> commandTokens = tokenize_command_line(trimmed);
        if (commandTokens.empty()) {
            logger.diagnostic(stdin_syntax_diagnostic(lineNumber, trimmed));
            exitCode = 1;
            continue;
        }

        const std::vector<std::string> mergedTokens = merged_stream_command_arguments(commandTokens, inheritedArguments);
        const ReqPackConfigOverrides mergedOverrides = extract_cli_config_overrides(mergedTokens);
        if (mergedOverrides.errorMessage.has_value()) {
            logger.diagnostic(config_override_diagnostic("stdin line " + std::to_string(lineNumber) + ": " + mergedOverrides.errorMessage.value()));
            exitCode = 1;
            continue;
        }
        const ReqPackConfig effectiveConfig = apply_config_overrides(config, mergedOverrides);
        const std::vector<Request> requests = cli.parse(mergedTokens, effectiveConfig);
        if (requests.empty()) {
            if (!cli.lastParseError().empty()) {
                logger.diagnostic(config_override_diagnostic("stdin line " + std::to_string(lineNumber) + ": " + cli.lastParseError()));
                exitCode = 1;
                continue;
            }
            logger.diagnostic(stdin_parse_diagnostic(lineNumber, trimmed));
            exitCode = 1;
            continue;
        }

        Orchestrator orchestrator(requests, effectiveConfig);
        if (orchestrator.run() != 0) {
            exitCode = 1;
        }
        logger.flushSync();
    }

    return exitCode;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::vector<std::string> earlyArguments;
    earlyArguments.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) {
        earlyArguments.emplace_back(argv[i]);
    }

    const PluginTestCliParseResult earlyPluginTest = parse_plugin_test_invocation(earlyArguments);
    if (earlyPluginTest.matched && earlyPluginTest.helpRequested) {
        print_plugin_test_help(std::cout);
        return 0;
    }

    // Fast path: handle -h/--help before any heavy initialisation.
    // This avoids CLI11 internal-state issues and the async Logger worker hang.
    {
        Cli earlyCliCheck;
        if (earlyCliCheck.handleHelp(argc, argv)) {
            return 0;
        }
    }

    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
        return 1;
    }

    Cli cli;
    const ReqPackConfigOverrides configOverrides = cli.parseConfigOverrides(argc, argv);
    if (configOverrides.errorMessage.has_value()) {
        Logger& logger = Logger::instance();
        logger.diagnostic(config_override_diagnostic(configOverrides.errorMessage.value()));
        logger.flushSync();
        curl_global_cleanup();
        return 1;
    }
    const std::filesystem::path configPath = configOverrides.configPath.value_or(default_reqpack_config_path());
    ReqPackConfig defaults = default_reqpack_config();
    defaults.registry.remoteUrl = DEFAULT_MAIN_REGISTRY_URL;
    const ReqPackConfig fileConfig = load_config_from_lua(configPath, defaults);
    ReqPackConfig config = apply_config_overrides(fileConfig, configOverrides);
    const std::filesystem::path workspacePluginDirectory = std::filesystem::current_path() / "plugins";
    if (!configOverrides.pluginDirectory.has_value() &&
        !configOverrides.configPath.has_value() &&
        config.registry.pluginDirectory == defaults.registry.pluginDirectory &&
        std::filesystem::exists(workspacePluginDirectory)) {
        config.registry.pluginDirectory = workspacePluginDirectory.string();
    }
    Logger& logger = Logger::instance();

    configure_logger_from_config(logger, config);

    std::unique_ptr<IDisplay> display = create_display(config.display);
    logger.setDisplay(display.get());

    std::vector<std::string> rawArguments;
    rawArguments.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) {
        rawArguments.emplace_back(argv[i]);
    }

    ServeRuntimeOptions serveOptions;
    std::string serveError;
    const bool isServeCommand = parse_serve_runtime_options(rawArguments, serveOptions, serveError);
    if (isServeCommand) {
        if (!serveError.empty()) {
            logger.diagnostic(make_error_diagnostic(
                "remote",
                "Serve command is invalid",
                "Remote or stdin serve options could not be parsed.",
                "Check serve flags and values, then run `rqp serve --help`.",
                serveError,
                "serve",
                "remote"
            ));
            logger.flushSync();
            curl_global_cleanup();
            return 1;
        }
        if (!serveOptions.readonlyExplicit) {
            serveOptions.readonly = config.remote.readonly;
        }
        if (!serveOptions.maxConnectionsExplicit) {
            serveOptions.maxConnections = config.remote.maxConnections;
        }
        if (serveOptions.stdin) {
            const int result = run_stdin_serve_loop(cli, config, serveOptions.inheritedArguments);
            curl_global_cleanup();
            return result;
        }
        const int result = run_remote_serve(cli, config, configPath, configOverrides, logger, display.get(), serveOptions);
        curl_global_cleanup();
        return result;
    }

    RemoteClientInvocation remoteInvocation;
    std::string remoteError;
    const bool isRemoteCommand = parse_remote_client_invocation(rawArguments, remoteInvocation, remoteError);
    if (isRemoteCommand) {
        if (!remoteError.empty()) {
            logger.diagnostic(make_error_diagnostic(
                "remote",
                "Remote command is invalid",
                "Remote client invocation could not be parsed.",
                "Check remote profile name and forwarded command syntax, then run `rqp remote --help`.",
                remoteError,
                "remote",
                "client"
            ));
            logger.flushSync();
            curl_global_cleanup();
            return 1;
        }

        int result = 1;
        try {
			result = run_remote_client(config, default_remote_profiles_path(), remoteInvocation.profileName, remoteInvocation.forwardedArguments, display.get());
        } catch (const std::exception& e) {
            logger.diagnostic(make_error_diagnostic(
                "remote",
                "Remote command execution failed",
                "ReqPack could not complete request against configured remote profile.",
                "Verify remote profile settings, connectivity, and authentication.",
                e.what(),
                remoteInvocation.profileName,
                "client"
            ));
            logger.flushSync();
            curl_global_cleanup();
            return 1;
        }

        curl_global_cleanup();
        return result;
    }

    if (is_install_stdin_command(rawArguments)) {
        const int result = run_stdin_install_batch(cli, config, inherited_stream_arguments(rawArguments));
        curl_global_cleanup();
        return result;
    }

    if (is_self_update_command(rawArguments)) {
        const int result = run_self_update(config, logger);
        logger.flushSync();
        curl_global_cleanup();
        return result;
    }

    if (is_host_refresh_command(rawArguments)) {
        const int result = run_host_refresh(logger);
        logger.flushSync();
        curl_global_cleanup();
        return result;
    }

    const PluginTestCliParseResult pluginTest = parse_plugin_test_invocation(rawArguments);
    if (pluginTest.matched) {
        if (pluginTest.helpRequested) {
            print_plugin_test_help(std::cout);
            logger.flushSync();
            curl_global_cleanup();
            return 0;
        }
        if (!pluginTest.error.empty()) {
            logger.diagnostic(plugin_test_diagnostic(
                "Plugin test invocation is invalid",
                "Required `test-plugin` arguments were missing or malformed.",
                "Run `rqp test-plugin --help` and retry with a plugin plus one or more case files.",
                pluginTest.error
            ));
            logger.flushSync();
            curl_global_cleanup();
            return 1;
        }

        try {
            const PluginTestRunReport report = run_plugin_test_cases(config, pluginTest.invocation);
            print_plugin_test_report(report, std::cout);
            logger.flushSync();
            curl_global_cleanup();
            return report.failed == 0 ? 0 : 1;
        } catch (const std::exception& error) {
            logger.diagnostic(plugin_test_diagnostic(
                "Plugin test execution failed",
                "ReqPack could not execute requested hermetic plugin test suite.",
                "Check plugin path, case files, and fake execution rules, then retry.",
                error.what()
            ));
            logger.flushSync();
            curl_global_cleanup();
            return 1;
        }
    }

    const std::vector<Request> requests = cli.parse(argc, argv, config);

    if (requests.empty()) {
        if (cli.parseFailed()) {
            if (!cli.lastParseError().empty()) {
                logger.diagnostic(config_override_diagnostic(cli.lastParseError()));
            }
            logger.flushSync();
            curl_global_cleanup();
            return 1;
        }
        cli.print_help();
        logger.flushSync();
        curl_global_cleanup();
        return 0;
    }

    Orchestrator orchestrator(requests, config);
    const int result = orchestrator.run();
    logger.flushSync();

    curl_global_cleanup();
    return result;
}
