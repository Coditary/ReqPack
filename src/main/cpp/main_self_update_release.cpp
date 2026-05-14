#include "main_self_update_internal.h"

#include "core/common/network_environment.h"
#include "core/registry/registry_database.h"
#include "output/logger.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <curl/curl.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <poll.h>
#include <set>
#include <spawn.h>
#include <sstream>
#include <string>
#include <system_error>
#include <sys/wait.h>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

constexpr const char* DEFAULT_SELF_UPDATE_RELEASE_API_BASE_URL = "https://api.github.com";

using boost::property_tree::ptree;
using self_update_internal::ensure_directory;
using self_update_internal::run_process;
using self_update_internal::shell_escape_arg;
using self_update_internal::split_non_empty_lines;
using self_update_internal::trim_copy;
using self_update_internal::trim_line;

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

std::string self_update_expected_asset_name(const std::string& tagName, const std::string& releaseTarget) {
    return "rqp-" + tagName + "-" + releaseTarget + ".tar.gz";
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

std::string normalize_archive_entry_name(std::string value) {
    value = trim_copy(value);
    while (value.rfind("./", 0) == 0) {
        value.erase(0, 2);
    }
    return value;
}

std::optional<std::string> detect_archive_entry_from_tar_output(
    const std::string& rawLine,
    const std::set<std::string>& expectedEntries
) {
    std::vector<std::string> candidates;
    const std::string trimmed = trim_copy(rawLine);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    candidates.push_back(trimmed);
    if (trimmed.rfind("x ", 0) == 0) {
        candidates.push_back(trim_copy(trimmed.substr(2)));
    }

    for (std::string candidate : candidates) {
        if (const std::size_t comma = candidate.find(','); comma != std::string::npos) {
            candidate = trim_copy(candidate.substr(0, comma));
        }
        candidate = normalize_archive_entry_name(candidate);
        if (candidate.empty()) {
            continue;
        }
        if (expectedEntries.empty()) {
            return candidate;
        }
        if (expectedEntries.find(candidate) != expectedEntries.end()) {
            return candidate;
        }
        if (!candidate.empty() && candidate.back() == '/') {
            const std::string withoutSlash = candidate.substr(0, candidate.size() - 1);
            if (expectedEntries.find(withoutSlash) != expectedEntries.end()) {
                return withoutSlash;
            }
            continue;
        }
        if (expectedEntries.find(candidate + "/") != expectedEntries.end()) {
            return candidate + "/";
        }
    }
    return std::nullopt;
}

std::vector<std::string> list_release_archive_entries(const std::filesystem::path& archivePath) {
    FILE* pipe = ::popen(("tar -tzf " + shell_escape_arg(archivePath.string())).c_str(), "r");
    if (pipe == nullptr) {
        return {};
    }

    std::string output;
    char buffer[4096];
    while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr) {
        output += buffer;
    }

    if (::pclose(pipe) != 0) {
        return {};
    }
    return split_non_empty_lines(output);
}

}  // namespace

namespace self_update_internal {

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

std::string self_update_download_failure_details(const std::string& url, const DownloadFailureDetails& failureDetails) {
    std::ostringstream details;
    bool emitted = false;
    const auto append = [&](const std::string& line) {
        if (line.empty()) {
            return;
        }
        if (emitted) {
            details << '\n';
        }
        details << line;
        emitted = true;
    };

    append("url: " + url);
    if (failureDetails.httpStatus > 0) {
        append("http status: " + std::to_string(failureDetails.httpStatus));
    }
    if (failureDetails.curlCode != CURLE_OK) {
        append("curl: " + std::to_string(static_cast<int>(failureDetails.curlCode)) +
               " (" + std::string{curl_easy_strerror(failureDetails.curlCode)} + ")");
    }
    if (!failureDetails.message.empty()) {
        append("details: " + failureDetails.message);
    }
    if (url.rfind("https://", 0) == 0) {
        const std::string caBundlePath = reqpack_ca_bundle_path();
        append("ca bundle: " + (caBundlePath.empty() ? std::string{"not found"} : caBundlePath));
    }
    return details.str();
}

std::string self_update_missing_asset_details(const SelfUpdateConfig& config,
                                             const std::filesystem::path& metadataPath,
                                             const std::string& releaseTarget) {
    std::ostringstream details;
    bool emitted = false;
    const auto append = [&](const std::string& line) {
        if (line.empty()) {
            return;
        }
        if (emitted) {
            details << '\n';
        }
        details << line;
        emitted = true;
    };

    const std::optional<ptree> tree = load_json_tree(metadataPath);
    const std::string tagName = tree.has_value() ? trim_copy(tree->get<std::string>("tag_name", {})) : std::string{};
    const std::string configuredTag = release_tag_for_config(config);
    const std::string effectiveTag = tagName.empty() ? configuredTag : tagName;
    const std::string assetTag = effectiveTag == "latest" ? std::string{"<tag>"} : effectiveTag;

    append("release tag: " + effectiveTag);
    append("release target: " + releaseTarget);
    append("expected asset: " + self_update_expected_asset_name(assetTag, releaseTarget));

    if (tree.has_value()) {
        const boost::optional<const ptree&> assets = tree->get_child_optional("assets");
        if (assets) {
            std::vector<std::string> assetNames;
            for (const auto& child : assets.get()) {
                const std::string assetName = trim_copy(child.second.get<std::string>("name", {}));
                if (!assetName.empty()) {
                    assetNames.push_back(assetName);
                }
            }
            if (!assetNames.empty()) {
                std::ostringstream available;
                available << "available assets: ";
                for (std::size_t index = 0; index < assetNames.size(); ++index) {
                    if (index != 0) {
                        available << ", ";
                    }
                    available << assetNames[index];
                }
                append(available.str());
            }
        }
    }

    return details.str();
}

std::optional<std::pair<std::string, std::string>> resolve_self_update_asset(
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

    const std::string expectedAssetName = self_update_expected_asset_name(tagName, releaseTarget);
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

bool extract_release_archive(const std::filesystem::path& archivePath,
                            const std::filesystem::path& destinationPath,
                            const std::function<void(std::size_t, std::size_t)>& onEntryExtracted) {
    if (!ensure_directory(destinationPath)) {
        return false;
    }

    const std::vector<std::string> entries = list_release_archive_entries(archivePath);
    const std::size_t totalEntries = entries.empty() ? 0 : entries.size();
    std::set<std::string> expectedEntries;
    for (const std::string& entry : entries) {
        const std::string normalized = normalize_archive_entry_name(entry);
        if (!normalized.empty()) {
            expectedEntries.insert(normalized);
        }
    }
    if (!onEntryExtracted) {
        return run_process({"tar", "-xzf", archivePath.string(), "-C", destinationPath.string()}, std::filesystem::current_path());
    }

    int stdoutPipe[2];
    int stderrPipe[2];
    if (::pipe(stdoutPipe) != 0) {
        return false;
    }
    if (::pipe(stderrPipe) != 0) {
        (void)::close(stdoutPipe[0]);
        (void)::close(stdoutPipe[1]);
        return false;
    }

    posix_spawn_file_actions_t fileActions;
    if (posix_spawn_file_actions_init(&fileActions) != 0) {
        (void)::close(stdoutPipe[0]);
        (void)::close(stdoutPipe[1]);
        (void)::close(stderrPipe[0]);
        (void)::close(stderrPipe[1]);
        return false;
    }

    bool ready =
        posix_spawn_file_actions_addopen(&fileActions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) == 0 &&
        posix_spawn_file_actions_adddup2(&fileActions, stdoutPipe[1], STDOUT_FILENO) == 0 &&
        posix_spawn_file_actions_adddup2(&fileActions, stderrPipe[1], STDERR_FILENO) == 0 &&
        posix_spawn_file_actions_addclose(&fileActions, stdoutPipe[0]) == 0 &&
        posix_spawn_file_actions_addclose(&fileActions, stdoutPipe[1]) == 0 &&
        posix_spawn_file_actions_addclose(&fileActions, stderrPipe[0]) == 0 &&
        posix_spawn_file_actions_addclose(&fileActions, stderrPipe[1]) == 0;
#if defined(__linux__) || defined(__APPLE__)
    if (ready && !std::filesystem::current_path().empty()) {
        ready = posix_spawn_file_actions_addchdir_np(&fileActions, std::filesystem::current_path().c_str()) == 0;
    }
#endif
    if (!ready) {
        posix_spawn_file_actions_destroy(&fileActions);
        (void)::close(stdoutPipe[0]);
        (void)::close(stdoutPipe[1]);
        (void)::close(stderrPipe[0]);
        (void)::close(stderrPipe[1]);
        return false;
    }

    const std::vector<std::string> arguments{"tar", "-xzvf", archivePath.string(), "-C", destinationPath.string()};
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
        (void)::close(stdoutPipe[0]);
        (void)::close(stderrPipe[0]);
        return false;
    }

    int stdoutFlags = fcntl(stdoutPipe[0], F_GETFL, 0);
    int stderrFlags = fcntl(stderrPipe[0], F_GETFL, 0);
    if (stdoutFlags >= 0) {
        (void)fcntl(stdoutPipe[0], F_SETFL, stdoutFlags | O_NONBLOCK);
    }
    if (stderrFlags >= 0) {
        (void)fcntl(stderrPipe[0], F_SETFL, stderrFlags | O_NONBLOCK);
    }

    std::string stdoutBuffer;
    std::string stderrBuffer;
    std::size_t extractedEntries = 0;
    std::set<std::string> extractedEntryNames;
    const auto record_entry = [&](const std::string& line) {
        const std::optional<std::string> entry = detect_archive_entry_from_tar_output(line, expectedEntries);
        if (!entry.has_value()) {
            return;
        }
        if (!extractedEntryNames.insert(entry.value()).second) {
            return;
        }
        extractedEntries = extractedEntryNames.size();
        onEntryExtracted(std::min(extractedEntries, totalEntries == 0 ? extractedEntries : totalEntries), totalEntries == 0 ? extractedEntries : totalEntries);
    };
    auto drain_pipe = [&](int fd, std::string& buffer, bool trackEntries) {
        char chunk[4096];
        while (true) {
            const ssize_t bytesRead = ::read(fd, chunk, sizeof(chunk));
            if (bytesRead > 0) {
                buffer.append(chunk, static_cast<std::size_t>(bytesRead));
                std::size_t newline = std::string::npos;
                while ((newline = buffer.find('\n')) != std::string::npos) {
                    std::string line = buffer.substr(0, newline);
                    buffer.erase(0, newline + 1);
                    if (!trackEntries) {
                        continue;
                    }
                    record_entry(line);
                }
                continue;
            }
            if (bytesRead == 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            break;
        }
    };

    pollfd fds[2] = {
        {.fd = stdoutPipe[0], .events = POLLIN},
        {.fd = stderrPipe[0], .events = POLLIN},
    };

    bool stdoutOpen = true;
    bool stderrOpen = true;
    while (stdoutOpen || stderrOpen) {
        const int pollResult = ::poll(fds, 2, -1);
        if (pollResult < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (stdoutOpen && (fds[0].revents & (POLLIN | POLLHUP))) {
            const std::size_t before = stdoutBuffer.size();
            drain_pipe(stdoutPipe[0], stdoutBuffer, true);
            if ((fds[0].revents & POLLHUP) && stdoutBuffer.size() == before) {
                stdoutOpen = false;
            }
        }
        if (stderrOpen && (fds[1].revents & (POLLIN | POLLHUP))) {
            const std::size_t before = stderrBuffer.size();
            drain_pipe(stderrPipe[0], stderrBuffer, true);
            if ((fds[1].revents & POLLHUP) && stderrBuffer.size() == before) {
                stderrOpen = false;
            }
        }
        if (stdoutOpen && (fds[0].revents & (POLLERR | POLLNVAL))) {
            stdoutOpen = false;
        }
        if (stderrOpen && (fds[1].revents & (POLLERR | POLLNVAL))) {
            stderrOpen = false;
        }
    }

    drain_pipe(stdoutPipe[0], stdoutBuffer, true);
    drain_pipe(stderrPipe[0], stderrBuffer, true);
    record_entry(stdoutBuffer);
    record_entry(stderrBuffer);

    (void)::close(stdoutPipe[0]);
    (void)::close(stderrPipe[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            return false;
        }
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && totalEntries > 0 && extractedEntries < totalEntries) {
        onEntryExtracted(totalEntries, totalEntries);
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
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

bool refresh_update_registry(const ReqPackConfig& config, Logger& logger, bool warnOnFailure) {
    ReqPackConfig mainRegistryConfig = config;
    mainRegistryConfig.registry.sources.clear();
    mainRegistryConfig.downloader.pluginSources.clear();
    RegistryDatabase registryDatabase(mainRegistryConfig);
    if (registryDatabase.refreshMainRegistry()) {
        return true;
    }

    if (warnOnFailure) {
        logger.diagnostic(make_warning_diagnostic(
            "registry",
            "Registry refresh failed before update",
            "ReqPack could not synchronize the main registry before running update and will continue with cached registry data if available.",
            "Check registry.remoteUrl, network access, and local registry cache if update results look stale.",
            {},
            "registry",
            "update"
        ));
    }
    return false;
}

}  // namespace self_update_internal
