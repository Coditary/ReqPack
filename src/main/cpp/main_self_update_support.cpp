#include "main_self_update_internal.h"

#include "core/common/network_environment.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace self_update_internal {

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

}  // namespace self_update_internal

namespace {

bool copy_file_with_mode(const std::filesystem::path& source, const std::filesystem::path& target) {
    std::error_code error;
    if (!self_update_internal::ensure_directory(target.parent_path())) {
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

}  // namespace

namespace self_update_internal {

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

std::string shell_escape_arg(const std::string& value) {
    std::string escaped{"'"};
    for (char ch : value) {
        if (ch == '\'') {
            escaped += "'\\''";
            continue;
        }
        escaped.push_back(ch);
    }
    escaped.push_back('\'');
    return escaped;
}

std::vector<std::string> split_non_empty_lines(const std::string& value) {
    std::vector<std::string> lines;
    std::istringstream stream(value);
    std::string line;
    while (std::getline(stream, line)) {
        if (const std::optional<std::string> trimmed = trim_line(line); trimmed.has_value()) {
            lines.push_back(trimmed.value());
        }
    }
    return lines;
}

}  // namespace self_update_internal
