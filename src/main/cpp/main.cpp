#include "cli/cli.h"
#include "core/configuration.h"
#include "core/host_info.h"
#include "core/orchestrator.h"
#include "core/remote_client.h"
#include "core/serve_remote.h"
#include "output/display_factory.h"
#include "output/logger.h"
#include "plugins/lua_bridge.h"

#include <curl/curl.h>

#include <cctype>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <fcntl.h>
#include <set>
#include <spawn.h>
#include <sstream>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

constexpr const char* DEFAULT_MAIN_REGISTRY_URL = "https://github.com/Coditary/rqp-registry.git";

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

std::string escape_shell_arg(const std::string& value) {
    std::string escaped{"'"};
    for (char c : value) {
        if (c == '\'') {
            escaped += "'\\''";
        } else {
            escaped.push_back(c);
        }
    }
    escaped.push_back('\'');
    return escaped;
}

int normalize_exit_code(const int status) {
    if (status == -1) {
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return status;
}

struct ProcessResult {
    int exitCode{-1};
    std::string output;
};

ProcessResult run_command_capture_status(const std::string& command) {
    int outputPipe[2];
    if (::pipe(outputPipe) != 0) {
        return {};
    }

    const int devNull = ::open("/dev/null", O_RDONLY);
    if (devNull < 0) {
        (void)::close(outputPipe[0]);
        (void)::close(outputPipe[1]);
        return {};
    }

    const pid_t child = ::fork();
    if (child < 0) {
        (void)::close(devNull);
        (void)::close(outputPipe[0]);
        (void)::close(outputPipe[1]);
        return {};
    }

    if (child == 0) {
        (void)::dup2(devNull, STDIN_FILENO);
        (void)::dup2(outputPipe[1], STDOUT_FILENO);
        (void)::dup2(outputPipe[1], STDERR_FILENO);
        (void)::close(devNull);
        (void)::close(outputPipe[0]);
        (void)::close(outputPipe[1]);
        ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    (void)::close(devNull);
    (void)::close(outputPipe[1]);

    std::string output;
    char buffer[4096];
    while (true) {
        const ssize_t bytesRead = ::read(outputPipe[0], buffer, sizeof(buffer));
        if (bytesRead > 0) {
            output.append(buffer, static_cast<std::size_t>(bytesRead));
            continue;
        }
        if (bytesRead == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        break;
    }
    (void)::close(outputPipe[0]);

    int status = -1;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            break;
        }
    }

    return ProcessResult{.exitCode = normalize_exit_code(status), .output = std::move(output)};
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

std::optional<std::string> trim_line(const std::string& value) {
    const std::string trimmed = trim_copy(value);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    return trimmed;
}

std::optional<std::string> git_current_commit(const std::filesystem::path& repoPath) {
    const ProcessResult result = run_command_capture_status(
        "git -C " + escape_shell_arg(repoPath.string()) + " rev-parse --verify HEAD"
    );
    if (result.exitCode != 0) {
        return std::nullopt;
    }
    return trim_line(result.output);
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

std::string binary_name_for_commit(const std::string& commit) {
    return "rqp-" + commit;
}

bool sync_self_update_repository(const SelfUpdateConfig& config, Logger& logger) {
    const std::filesystem::path repoPath(config.repoPath);
    if (!ensure_directory(repoPath.parent_path())) {
        logger.err("failed to create self-update repo parent directory");
        return false;
    }

    const std::filesystem::path gitDirectory = repoPath / ".git";
    if (!std::filesystem::exists(gitDirectory)) {
        logger.stdout("self-update: clone repository");
        if (!run_process({
                "git", "clone", "--branch", config.branch, "--single-branch", config.repoUrl, repoPath.string()
            }, std::filesystem::current_path())) {
            logger.err("self-update clone failed");
            return false;
        }
        return true;
    }

    logger.stdout("self-update: checkout branch");
    if (!run_process({"git", "checkout", config.branch}, repoPath)) {
        logger.err("self-update checkout failed");
        return false;
    }
    logger.stdout("self-update: pull latest commit");
    if (!run_process({"git", "pull", "--ff-only", "origin", config.branch}, repoPath)) {
        logger.err("self-update fast-forward failed");
        return false;
    }
    return true;
}

bool build_self_update_binary(const ReqPackConfig& config, const std::string& commit, std::filesystem::path& installedBinaryPath, Logger& logger) {
    const std::filesystem::path repoPath(config.selfUpdate.repoPath);
    const std::filesystem::path buildPath = std::filesystem::path(config.selfUpdate.buildPath) / commit;
    const std::filesystem::path outputBinary = std::filesystem::path(config.selfUpdate.binaryDirectory) / binary_name_for_commit(commit);
    const std::filesystem::path candidateBinary = buildPath / "ReqPack";

    installedBinaryPath = outputBinary;
    if (std::filesystem::exists(outputBinary)) {
        logger.stdout("self-update: binary already built for commit " + commit);
        return true;
    }

    if (!ensure_directory(buildPath) || !ensure_directory(outputBinary.parent_path())) {
        logger.err("failed to create self-update build directories");
        return false;
    }

    logger.stdout("self-update: configure build");
    if (!run_process({
            "cmake",
            "-S", repoPath.string(),
            "-B", buildPath.string(),
            "-DCMAKE_BUILD_TYPE=Release"
        }, repoPath)) {
        logger.err("self-update configure failed");
        return false;
    }

    logger.stdout("self-update: build binary");
    if (!run_process({"cmake", "--build", buildPath.string(), "--target", "ReqPack"}, repoPath)) {
        logger.err("self-update build failed");
        return false;
    }

    if (!std::filesystem::exists(candidateBinary)) {
        logger.err("self-update build produced no ReqPack binary");
        return false;
    }
    if (!copy_file_with_mode(candidateBinary, outputBinary)) {
        logger.err("failed to install built self-update binary");
        return false;
    }
    return true;
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
        logger.err("self-update repoUrl is not configured");
        return 1;
    }
    if (config.selfUpdate.branch.empty()) {
        logger.err("self-update branch is not configured");
        return 1;
    }
    if (config.selfUpdate.repoPath.empty() || config.selfUpdate.buildPath.empty() ||
        config.selfUpdate.binaryDirectory.empty() || config.selfUpdate.linkPath.empty()) {
        logger.err("self-update paths are not fully configured");
        return 1;
    }

    const std::filesystem::path repoPath(config.selfUpdate.repoPath);
    const std::optional<std::string> previousCommit = std::filesystem::exists(repoPath / ".git")
        ? git_current_commit(repoPath)
        : std::nullopt;

    if (!sync_self_update_repository(config.selfUpdate, logger)) {
        return 1;
    }

    const std::optional<std::string> currentCommit = git_current_commit(repoPath);
    if (!currentCommit.has_value()) {
        logger.err("failed to resolve self-update commit");
        return 1;
    }

    std::filesystem::path installedBinaryPath;
    if (!build_self_update_binary(config, currentCommit.value(), installedBinaryPath, logger)) {
        return 1;
    }

    logger.stdout("self-update: update local symlink");
    if (!replace_symlink_atomically(installedBinaryPath, std::filesystem::path(config.selfUpdate.linkPath))) {
        logger.err("failed to update self-update symlink");
        return 1;
    }
    if (!HostInfoService::invalidateCache()) {
        logger.warn("self-update: failed to invalidate host info cache");
    }

    if (previousCommit.has_value() && previousCommit.value() == currentCommit.value()) {
        logger.stdout("self-update complete: already on latest commit " + currentCommit.value());
    } else {
        logger.stdout("self-update complete: now on commit " + currentCommit.value());
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
        logger.err(inheritedOverrides.errorMessage.value());
        return 1;
    }
    ReqPackConfig effectiveConfig = apply_config_overrides(config, inheritedOverrides);

    for (const StdinCommand& command : commands) {
        const std::vector<std::string> commandTokens = tokenize_command_line(command.text);
        if (commandTokens.empty()) {
            logger.err("stdin line " + std::to_string(command.lineNumber) + ": invalid command syntax");
            return 1;
        }

        const std::vector<std::string> mergedTokens = merged_stream_command_arguments(commandTokens, inheritedArguments);
        const ReqPackConfigOverrides mergedOverrides = extract_cli_config_overrides(mergedTokens);
        if (mergedOverrides.errorMessage.has_value()) {
            logger.err("stdin line " + std::to_string(command.lineNumber) + ": " + mergedOverrides.errorMessage.value());
            return 1;
        }
        effectiveConfig = apply_config_overrides(effectiveConfig, mergedOverrides);
        const std::vector<Request> parsed = cli.parse(mergedTokens, effectiveConfig);
        if (parsed.empty()) {
            if (!cli.lastParseError().empty()) {
                logger.err("stdin line " + std::to_string(command.lineNumber) + ": " + cli.lastParseError());
                return 1;
            }
            logger.err("stdin line " + std::to_string(command.lineNumber) + ": failed to parse '" + command.text + "'");
            return 1;
        }

        for (const Request& request : parsed) {
            if (request.action != ActionType::INSTALL) {
                logger.err("stdin line " + std::to_string(command.lineNumber) + ": only install commands allowed in 'install --stdin'");
                return 1;
            }
            requests.push_back(request);
        }
    }

    if (requests.empty()) {
        logger.err("stdin contained no install commands");
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
            logger.err("stdin line " + std::to_string(lineNumber) + ": invalid command syntax");
            exitCode = 1;
            continue;
        }

        const std::vector<std::string> mergedTokens = merged_stream_command_arguments(commandTokens, inheritedArguments);
        const ReqPackConfigOverrides mergedOverrides = extract_cli_config_overrides(mergedTokens);
        if (mergedOverrides.errorMessage.has_value()) {
            logger.err("stdin line " + std::to_string(lineNumber) + ": " + mergedOverrides.errorMessage.value());
            exitCode = 1;
            continue;
        }
        const ReqPackConfig effectiveConfig = apply_config_overrides(config, mergedOverrides);
        const std::vector<Request> requests = cli.parse(mergedTokens, effectiveConfig);
        if (requests.empty()) {
            if (!cli.lastParseError().empty()) {
                logger.err("stdin line " + std::to_string(lineNumber) + ": " + cli.lastParseError());
                exitCode = 1;
                continue;
            }
            logger.err("stdin line " + std::to_string(lineNumber) + ": failed to parse '" + trimmed + "'");
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
        logger.err(configOverrides.errorMessage.value());
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

    logger.setLevel(to_string(config.logging.level));
    logger.setPattern(config.logging.pattern);
    logger.setBacktrace(config.logging.enableBacktrace, config.logging.backtraceSize);
    logger.setConsoleOutput(config.logging.consoleOutput);
    if (config.logging.fileOutput) {
        logger.setFileSink(config.logging.filePath);
    }

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
            logger.err(serveError);
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
            logger.err(remoteError);
            logger.flushSync();
            curl_global_cleanup();
            return 1;
        }

        int result = 1;
        try {
			result = run_remote_client(config, default_remote_profiles_path(), remoteInvocation.profileName, remoteInvocation.forwardedArguments, display.get());
        } catch (const std::exception& e) {
            logger.err(e.what());
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

    const std::vector<Request> requests = cli.parse(argc, argv, config);

    if (requests.empty()) {
        if (cli.parseFailed()) {
            if (!cli.lastParseError().empty()) {
                logger.err(cli.lastParseError());
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
