#include "registry_database_internal.h"

#include "core/common/process_runner.h"
#include "core/common/version_compare.h"

#include <cctype>
#include <sstream>
#include <string_view>

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
    const ReqpackProcessResult captured = reqpack_run_process_capture(arguments);
    return ProcessResult{
        .exitCode = captured.exitCode,
        .stdoutText = captured.stdoutText,
        .stderrText = captured.stderrText,
    };
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
