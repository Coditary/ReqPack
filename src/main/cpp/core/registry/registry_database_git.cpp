#include "registry_database_internal.h"

#include "core/common/network_environment.h"
#include "core/common/version_compare.h"

#include <cerrno>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sstream>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct ProcessResult {
    int exitCode{1};
    std::string stdoutText{};
    std::string stderrText{};
};

bool run_process_quiet(const std::vector<std::string>& arguments);
std::optional<std::string> run_process_capture_stdout(const std::vector<std::string>& arguments);

std::string trim_copy(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

ProcessResult run_process_capture(const std::vector<std::string>& arguments) {
    ProcessResult result;
    if (arguments.empty()) {
        result.stderrText = "empty command";
        return result;
    }

    int stdoutPipe[2];
    if (::pipe(stdoutPipe) != 0) {
        result.stderrText = std::string{"pipe(stdout) failed: "} + std::strerror(errno);
        return result;
    }

    int stderrPipe[2];
    if (::pipe(stderrPipe) != 0) {
        result.stderrText = std::string{"pipe(stderr) failed: "} + std::strerror(errno);
        (void)::close(stdoutPipe[0]);
        (void)::close(stdoutPipe[1]);
        return result;
    }

    posix_spawn_file_actions_t fileActions;
    if (posix_spawn_file_actions_init(&fileActions) != 0) {
        result.stderrText = "posix_spawn_file_actions_init failed";
        (void)::close(stdoutPipe[0]);
        (void)::close(stdoutPipe[1]);
        (void)::close(stderrPipe[0]);
        (void)::close(stderrPipe[1]);
        return result;
    }

    const bool actionsReady =
        posix_spawn_file_actions_addopen(&fileActions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) == 0 &&
        posix_spawn_file_actions_adddup2(&fileActions, stdoutPipe[1], STDOUT_FILENO) == 0 &&
        posix_spawn_file_actions_adddup2(&fileActions, stderrPipe[1], STDERR_FILENO) == 0 &&
        posix_spawn_file_actions_addclose(&fileActions, stdoutPipe[0]) == 0 &&
        posix_spawn_file_actions_addclose(&fileActions, stdoutPipe[1]) == 0 &&
        posix_spawn_file_actions_addclose(&fileActions, stderrPipe[0]) == 0 &&
        posix_spawn_file_actions_addclose(&fileActions, stderrPipe[1]) == 0;
    if (!actionsReady) {
        posix_spawn_file_actions_destroy(&fileActions);
        result.stderrText = "failed to configure process pipes";
        (void)::close(stdoutPipe[0]);
        (void)::close(stdoutPipe[1]);
        (void)::close(stderrPipe[0]);
        (void)::close(stderrPipe[1]);
        return result;
    }

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const std::string& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);

    std::vector<std::string> environmentStorage = reqpack_sanitized_process_environment();
    std::vector<char*> environmentPointers;
    environmentPointers.reserve(environmentStorage.size() + 1);
    for (std::string& entry : environmentStorage) {
        environmentPointers.push_back(entry.data());
    }
    environmentPointers.push_back(nullptr);

    pid_t pid = 0;
    const int spawnResult = posix_spawnp(&pid, arguments.front().c_str(), &fileActions, nullptr, argv.data(), environmentPointers.data());
    posix_spawn_file_actions_destroy(&fileActions);
    (void)::close(stdoutPipe[1]);
    (void)::close(stderrPipe[1]);
    if (spawnResult != 0) {
        result.stderrText = std::string{"spawn failed: "} + std::strerror(spawnResult);
        (void)::close(stdoutPipe[0]);
        (void)::close(stderrPipe[0]);
        return result;
    }

    char buffer[4096];
    while (true) {
        const ssize_t bytesRead = ::read(stdoutPipe[0], buffer, sizeof(buffer));
        if (bytesRead > 0) {
            result.stdoutText.append(buffer, static_cast<std::size_t>(bytesRead));
            continue;
        }
        if (bytesRead == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        result.stderrText += std::string{"read(stdout) failed: "} + std::strerror(errno);
        break;
    }
    (void)::close(stdoutPipe[0]);

    while (true) {
        const ssize_t bytesRead = ::read(stderrPipe[0], buffer, sizeof(buffer));
        if (bytesRead > 0) {
            result.stderrText.append(buffer, static_cast<std::size_t>(bytesRead));
            continue;
        }
        if (bytesRead == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        result.stderrText += std::string{"read(stderr) failed: "} + std::strerror(errno);
        break;
    }
    (void)::close(stderrPipe[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            result.stderrText += std::string{"waitpid failed: "} + std::strerror(errno);
            return result;
        }
    }

    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
        return result;
    }

    if (WIFSIGNALED(status)) {
        result.exitCode = 128 + WTERMSIG(status);
        if (!result.stderrText.empty() && result.stderrText.back() != '\n') {
            result.stderrText.push_back('\n');
        }
        result.stderrText += "terminated by signal " + std::to_string(WTERMSIG(status));
        return result;
    }

    result.stderrText += "process ended unexpectedly";
    return result;
}

bool run_process_quiet(const std::vector<std::string>& arguments) {
    return run_process_capture(arguments).exitCode == 0;
}

std::optional<std::string> run_process_capture_stdout(const std::vector<std::string>& arguments) {
    const ProcessResult result = run_process_capture(arguments);
    if (result.exitCode != 0) {
        return std::nullopt;
    }
    return result.stdoutText;
}

std::optional<std::string> normalize_git_tag_for_compare(const std::string& tag) {
    const std::string trimmed = trim_copy(tag);
    if (trimmed.empty() || trimmed.ends_with("^{}")) {
        return std::nullopt;
    }

    std::string normalized = trimmed;
    if (!normalized.empty() && (normalized.front() == 'v' || normalized.front() == 'V') && normalized.size() > 1 &&
        std::isdigit(static_cast<unsigned char>(normalized[1])) != 0) {
        normalized.erase(normalized.begin());
    }

    bool hasDigit = false;
    for (unsigned char c : normalized) {
        if (std::isdigit(c) != 0) {
            hasDigit = true;
            continue;
        }
        if (std::isalpha(c) != 0 || c == '-' || c == '.' || c == '+') {
            continue;
        }
        return std::nullopt;
    }

    if (!hasDigit || normalized.empty()) {
        return std::nullopt;
    }

    return normalized;
}

}  // namespace

std::optional<std::string> git_repository_head_commit(const std::filesystem::path& repositoryPath) {
    const std::optional<std::string> output = run_process_capture_stdout({
        "git", "-C", repositoryPath.string(), "rev-parse", "--verify", "HEAD"
    });
    if (!output.has_value()) {
        return std::nullopt;
    }
    return trim_copy(output.value());
}

bool git_commit_exists(const std::filesystem::path& repositoryPath, const std::string& commit) {
    if (commit.empty()) {
        return false;
    }
    return run_process_quiet({
        "git", "-C", repositoryPath.string(), "rev-parse", "--verify", "--quiet", commit + "^{commit}"
    });
}

std::optional<std::vector<RegistryDiffEntry>> git_registry_diff(
    const std::filesystem::path& repositoryPath,
    const std::string& oldCommit,
    const std::string& newCommit,
    const std::string& pluginsPath
) {
    const std::optional<std::string> output = run_process_capture_stdout({
        "git",
        "-C",
        repositoryPath.string(),
        "diff",
        "--name-status",
        oldCommit + ".." + newCommit,
        "--",
        pluginsPath
    });
    if (!output.has_value()) {
        return std::nullopt;
    }

    std::vector<RegistryDiffEntry> entries;
    std::istringstream stream(output.value());
    std::string line;
    while (std::getline(stream, line)) {
        line = trim_copy(line);
        if (line.empty()) {
            continue;
        }

        std::istringstream lineStream(line);
        std::string status;
        std::string path;
        if (!(lineStream >> status)) {
            return std::nullopt;
        }
        if (status.empty()) {
            return std::nullopt;
        }

        if (status[0] == 'R' || status[0] == 'C') {
            std::string oldPath;
            std::string newPath;
            if (!(lineStream >> oldPath >> newPath)) {
                return std::nullopt;
            }
            entries.push_back({status[0], oldPath});
            entries.push_back({'A', newPath});
            continue;
        }

        if (!(lineStream >> path)) {
            return std::nullopt;
        }
        entries.push_back({status[0], path});
    }

    return entries;
}

std::optional<std::string> latest_git_tag_for_source(const std::string& source) {
    if (!registry_database_is_git_source(source)) {
        return std::nullopt;
    }

    const std::optional<std::string> output = run_process_capture_stdout({
        "git", "ls-remote", "--tags", "--refs", registry_database_git_source_url(source)
    });
    if (!output.has_value()) {
        return std::nullopt;
    }

    const std::vector<std::string> tags = registry_database_extract_git_tags(output.value());
    std::optional<std::string> bestTag;
    std::optional<std::string> bestNormalized;
    for (const std::string& tag : tags) {
        const std::optional<std::string> normalized = normalize_git_tag_for_compare(tag);
        if (!normalized.has_value()) {
            continue;
        }
        if (!bestNormalized.has_value() || version_compare_values(normalized.value(), bestNormalized.value(), VersionComparatorSpec{.profile = "semver"}) > 0) {
            bestTag = tag;
            bestNormalized = normalized;
        }
    }

    return bestTag;
}

bool sync_git_repository(const ReqPackConfig& config, const std::string& source, const std::string& pluginName, std::string* errorDetails) {
    const std::filesystem::path repositoryPath = registry_database_git_repository_cache_path(config, source, pluginName);
    const std::string repositoryUrl = registry_database_git_source_url(source);
    const std::string requestedRef = registry_database_git_source_ref(source);

    const auto set_error = [&](const std::string& step, const ProcessResult& processResult) {
        if (errorDetails == nullptr) {
            return;
        }
        std::ostringstream message;
        message << step << " failed for source '" << source << "'";
        if (!repositoryUrl.empty()) {
            message << "\nurl: " << repositoryUrl;
        }
        if (!requestedRef.empty()) {
            message << "\nref: " << requestedRef;
        }
        message << "\nexit code: " << processResult.exitCode;
        if (!trim_copy(processResult.stdoutText).empty()) {
            message << "\nstdout:\n" << trim_copy(processResult.stdoutText);
        }
        if (!trim_copy(processResult.stderrText).empty()) {
            message << "\nstderr:\n" << trim_copy(processResult.stderrText);
        }
        *errorDetails = message.str();
    };

    std::error_code directoryError;
    std::filesystem::create_directories(repositoryPath.parent_path(), directoryError);
    if (directoryError) {
        if (errorDetails != nullptr) {
            *errorDetails = "create_directories failed for '" + repositoryPath.parent_path().string() + "': " + directoryError.message();
        }
        return false;
    }

    const std::filesystem::path gitDirectory = repositoryPath / ".git";
    if (std::filesystem::exists(gitDirectory)) {
        if (requestedRef.empty()) {
            const ProcessResult pullResult = run_process_capture({"git", "-C", repositoryPath.string(), "pull", "--ff-only", "--quiet"});
            if (pullResult.exitCode == 0) {
                return true;
            }
            set_error("git pull", pullResult);
        } else {
            const ProcessResult fetchResult = run_process_capture({"git", "-C", repositoryPath.string(), "fetch", "--tags", "--quiet", "origin"});
            const bool fetched = fetchResult.exitCode == 0;
            ProcessResult checkoutResult;
            ProcessResult checkoutOriginResult;
            bool checkedOut = false;
            if (fetched) {
                checkoutResult = run_process_capture({"git", "-C", repositoryPath.string(), "checkout", "--quiet", requestedRef});
                checkedOut = checkoutResult.exitCode == 0;
                if (!checkedOut) {
                    checkoutOriginResult = run_process_capture({"git", "-C", repositoryPath.string(), "checkout", "--quiet", "origin/" + requestedRef});
                    checkedOut = checkoutOriginResult.exitCode == 0;
                }
            }
            if (checkedOut) {
                if (run_process_quiet({"git", "-C", repositoryPath.string(), "rev-parse", "--verify", "--quiet", "origin/" + requestedRef})) {
                    (void)run_process_quiet({"git", "-C", repositoryPath.string(), "reset", "--hard", "origin/" + requestedRef});
                }
                return true;
            }
            if (!fetched) {
                set_error("git fetch", fetchResult);
            } else if (checkoutResult.exitCode != 0) {
                set_error("git checkout", checkoutResult);
                if (checkoutOriginResult.exitCode != 0 && errorDetails != nullptr) {
                    *errorDetails += "\norigin checkout exit code: " + std::to_string(checkoutOriginResult.exitCode);
                    if (!trim_copy(checkoutOriginResult.stderrText).empty()) {
                        *errorDetails += "\norigin checkout stderr:\n" + trim_copy(checkoutOriginResult.stderrText);
                    }
                }
            }
        }

        std::error_code removeError;
        std::filesystem::remove_all(repositoryPath, removeError);
        if (removeError) {
            if (errorDetails != nullptr) {
                *errorDetails = "remove_all failed for stale repository '" + repositoryPath.string() + "': " + removeError.message();
            }
            return false;
        }
    } else if (std::filesystem::exists(repositoryPath)) {
        std::error_code removeError;
        std::filesystem::remove_all(repositoryPath, removeError);
        if (removeError) {
            if (errorDetails != nullptr) {
                *errorDetails = "remove_all failed for existing path '" + repositoryPath.string() + "': " + removeError.message();
            }
            return false;
        }
    }

    const ProcessResult cloneResult = run_process_capture({
        "git",
        "clone",
        "--quiet",
        repositoryUrl,
        repositoryPath.string()
    });
    if (cloneResult.exitCode != 0) {
        set_error("git clone", cloneResult);
        return false;
    }

    if (requestedRef.empty()) {
        return true;
    }

    const ProcessResult checkoutResult = run_process_capture({"git", "-C", repositoryPath.string(), "checkout", "--quiet", requestedRef});
    if (checkoutResult.exitCode == 0) {
        return true;
    }

    const ProcessResult checkoutOriginResult = run_process_capture({"git", "-C", repositoryPath.string(), "checkout", "--quiet", "origin/" + requestedRef});
    if (checkoutOriginResult.exitCode == 0) {
        return true;
    }

    set_error("git checkout", checkoutResult);
    if (errorDetails != nullptr) {
        *errorDetails += "\norigin checkout exit code: " + std::to_string(checkoutOriginResult.exitCode);
        if (!trim_copy(checkoutOriginResult.stderrText).empty()) {
            *errorDetails += "\norigin checkout stderr:\n" + trim_copy(checkoutOriginResult.stderrText);
        }
    }
    return false;
}
