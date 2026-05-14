#include "archive_resolver_internal.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <termios.h>
#include <sys/wait.h>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

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

class TerminalEchoGuard {
public:
    explicit TerminalEchoGuard(const int fd) : fd_(fd) {
        if (fd_ < 0 || ::tcgetattr(fd_, &original_) != 0) {
            return;
        }

        termios updated = original_;
        updated.c_lflag &= static_cast<unsigned int>(~ECHO);
        if (::tcsetattr(fd_, TCSAFLUSH, &updated) == 0) {
            active_ = true;
        }
    }

    ~TerminalEchoGuard() {
        if (active_) {
            (void)::tcsetattr(fd_, TCSAFLUSH, &original_);
        }
    }

    bool active() const {
        return active_;
    }

private:
    int fd_{-1};
    termios original_{};
    bool active_{false};
};

bool output_contains(const archive_resolver_internal::CommandResult& result, const std::string& token) {
    return archive_resolver_internal::to_lower_copy(result.output).find(archive_resolver_internal::to_lower_copy(token)) !=
           std::string::npos;
}

std::vector<std::string> parse_seven_zip_entries(const std::string& content) {
    std::vector<std::string> entries;
    std::istringstream input(content);
    std::string line;
    while (std::getline(input, line)) {
        line = archive_resolver_internal::trim_line(line);
        if (line.rfind("Path = ", 0) != 0) {
            continue;
        }

        const std::string path = line.substr(7);
        if (path.empty()) {
            continue;
        }
        if (path.find("/") == std::string::npos && path.find('\\') == std::string::npos &&
            path == archive_resolver_internal::trim_line(path) && path == path) {
            entries.push_back(path);
        } else {
            entries.push_back(path);
        }
    }

    if (!entries.empty()) {
        entries.erase(entries.begin());
    }
    return entries;
}

}  // namespace

namespace archive_resolver_internal {

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool is_single_file_archive_suffix(const std::string& suffix) {
    return suffix == ".gz" || suffix == ".bz2" || suffix == ".xz" || suffix == ".zst";
}

std::string single_file_archive_output_name(const std::filesystem::path& archivePath, const std::string& suffix) {
    const std::string filename = archivePath.filename().string();
    if (!is_single_file_archive_suffix(suffix) || filename.size() <= suffix.size()) {
        return "payload";
    }

    const std::string outputName = filename.substr(0, filename.size() - suffix.size());
    return outputName.empty() ? "payload" : outputName;
}

std::filesystem::path single_file_archive_output_path(
    const std::filesystem::path& archivePath,
    const std::filesystem::path& outputDirectory,
    const std::string& suffix
) {
    return outputDirectory / single_file_archive_output_name(archivePath, suffix);
}

std::filesystem::path make_unique_directory(const std::filesystem::path& root, const std::string& prefix) {
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) {
        throw std::runtime_error("failed to create archive temp root: " + root.string());
    }

    const std::filesystem::path pattern = root / (prefix + "-XXXXXX");
    std::string templateString = pattern.string();
    std::vector<char> buffer(templateString.begin(), templateString.end());
    buffer.push_back('\0');
    char* created = ::mkdtemp(buffer.data());
    if (created == nullptr) {
        throw std::runtime_error("failed to create archive temp directory");
    }
    return created;
}

std::filesystem::path make_unique_file_path(const std::filesystem::path& root, const std::string& stem, const std::string& suffix) {
    const std::filesystem::path tempDirectory = make_unique_directory(root, stem);
    std::filesystem::path filename = stem;
    if (!suffix.empty()) {
        filename += suffix;
    }
    return tempDirectory / filename;
}

std::string escape_shell_arg(const std::string& value) {
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

std::string trim_line(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

std::string run_command_capture(const std::string& command) {
    FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
        throw std::runtime_error("failed to inspect archive");
    }

    std::string output;
    char buffer[4096];
    while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr) {
        output += buffer;
    }

    if (::pclose(pipe) != 0) {
        throw std::runtime_error("failed to inspect archive");
    }
    return output;
}

CommandResult run_command_capture_status(const std::string& command) {
    int outputPipe[2];
    if (::pipe(outputPipe) != 0) {
        throw std::runtime_error("failed to run archive command");
    }

    const int devNull = ::open("/dev/null", O_RDONLY);
    if (devNull < 0) {
        (void)::close(outputPipe[0]);
        (void)::close(outputPipe[1]);
        throw std::runtime_error("failed to run archive command");
    }

    const pid_t child = ::fork();
    if (child < 0) {
        (void)::close(devNull);
        (void)::close(outputPipe[0]);
        (void)::close(outputPipe[1]);
        throw std::runtime_error("failed to run archive command");
    }

    if (child == 0) {
        (void)::setsid();
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

    return CommandResult{.exitCode = normalize_exit_code(status), .output = std::move(output)};
}

bool path_has_invalid_segments(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute()) {
        return true;
    }

    for (const std::filesystem::path& part : path) {
        const std::string token = part.string();
        if (token.empty() || token == "." || token == "..") {
            return true;
        }
    }
    return false;
}

std::vector<std::string> split_lines(const std::string& content) {
    std::vector<std::string> lines;
    std::istringstream input(content);
    std::string line;
    while (std::getline(input, line)) {
        line = trim_line(line);
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

bool zip_requires_password(const CommandResult& result) {
    return result.exitCode == 5 || output_contains(result, "unable to get password");
}

bool zip_has_invalid_password(const CommandResult& result) {
    return result.exitCode == 82 || output_contains(result, "incorrect password") ||
           output_contains(result, "may instead be incorrect password");
}

bool seven_zip_password_error(const CommandResult& result) {
    return output_contains(result, "cannot open encrypted archive. wrong password?") || output_contains(result, "headers error");
}

bool gpg_password_error(const CommandResult& result) {
    return output_contains(result, "decryption failed: bad session key");
}

std::optional<std::string> prompt_archive_password(const std::filesystem::path& archivePath) {
    std::cout << "Archive password required for " << archivePath.string() << ": ";
    std::cout.flush();

    std::string password;
    const int fd = ::fileno(stdin);
    if (fd >= 0 && ::isatty(fd)) {
        TerminalEchoGuard guard(fd);
        if (!std::getline(std::cin, password)) {
            if (guard.active()) {
                std::cout << '\n';
                std::cout.flush();
            }
            return std::nullopt;
        }
        if (guard.active()) {
            std::cout << '\n';
            std::cout.flush();
        }
        return password;
    }

    if (!std::getline(std::cin, password)) {
        return std::nullopt;
    }
    return password;
}

std::string archive_password_required_message(const std::filesystem::path& path) {
    return "archive password required: " + path.string();
}

std::string invalid_archive_password_message(const std::filesystem::path& path) {
    return "invalid archive password: " + path.string();
}

std::string zip_extract_command(
    const std::filesystem::path& archivePath,
    const std::filesystem::path& outputDirectory,
    const std::string& password
) {
    std::string command = "unzip -o ";
    if (!password.empty()) {
        command += "-P " + escape_shell_arg(password) + " ";
    }
    command += escape_shell_arg(archivePath.string()) + " -d " + escape_shell_arg(outputDirectory.string());
    return command;
}

std::string seven_zip_password_flag(const std::string& password) {
    return std::string("-p") + escape_shell_arg(password);
}

std::string seven_zip_extract_command(
    const std::filesystem::path& archivePath,
    const std::filesystem::path& outputDirectory,
    const std::string& password
) {
    return "7z x -y " + seven_zip_password_flag(password) + " -o" + escape_shell_arg(outputDirectory.string()) + " " +
           escape_shell_arg(archivePath.string());
}

std::vector<std::string> seven_zip_entries(
    const std::filesystem::path& archivePath,
    std::string password,
    const bool interactive,
    const std::filesystem::path& promptPath
) {
    bool canPrompt = interactive;

    while (true) {
        const CommandResult result = run_command_capture_status(
            "7z l -slt " + seven_zip_password_flag(password) + " " + escape_shell_arg(archivePath.string())
        );
        if (result.exitCode == 0) {
            return parse_seven_zip_entries(result.output);
        }

        if (seven_zip_password_error(result)) {
            if (canPrompt) {
                canPrompt = false;
                const std::optional<std::string> promptedPassword = prompt_archive_password(promptPath);
                if (promptedPassword.has_value()) {
                    password = promptedPassword.value();
                    continue;
                }
            }
            throw std::runtime_error(password.empty() ? archive_password_required_message(promptPath)
                                                      : invalid_archive_password_message(promptPath));
        }

        throw std::runtime_error("failed to inspect archive: " + promptPath.string());
    }
}

void extract_zip_archive_to_directory(
    const std::filesystem::path& archivePath,
    const std::filesystem::path& outputDirectory,
    const ArchiveExtractionOptions& options,
    const std::filesystem::path& promptPath
) {
    std::string password = options.password;
    bool canPrompt = options.interactive;

    while (true) {
        const CommandResult result = run_command_capture_status(zip_extract_command(archivePath, outputDirectory, password));
        if (result.exitCode == 0) {
            return;
        }

        const bool missingPassword = zip_requires_password(result);
        const bool invalidPassword = zip_has_invalid_password(result);
        if ((missingPassword || invalidPassword) && canPrompt) {
            canPrompt = false;
            const std::optional<std::string> promptedPassword = prompt_archive_password(promptPath);
            if (promptedPassword.has_value()) {
                password = promptedPassword.value();
                continue;
            }
        }

        if (missingPassword) {
            throw std::runtime_error(archive_password_required_message(promptPath));
        }
        if (invalidPassword) {
            throw std::runtime_error(invalid_archive_password_message(promptPath));
        }
        throw std::runtime_error("failed to extract archive: " + promptPath.string());
    }
}

void extract_seven_zip_archive_to_directory(
    const std::filesystem::path& archivePath,
    const std::filesystem::path& outputDirectory,
    const ArchiveExtractionOptions& options,
    const std::filesystem::path& promptPath
) {
    std::string password = options.password;
    bool canPrompt = options.interactive;

    while (true) {
        const CommandResult result = run_command_capture_status(seven_zip_extract_command(archivePath, outputDirectory, password));
        if (result.exitCode == 0) {
            return;
        }

        if (seven_zip_password_error(result)) {
            if (canPrompt) {
                canPrompt = false;
                const std::optional<std::string> promptedPassword = prompt_archive_password(promptPath);
                if (promptedPassword.has_value()) {
                    password = promptedPassword.value();
                    continue;
                }
            }
            throw std::runtime_error(password.empty() ? archive_password_required_message(promptPath)
                                                      : invalid_archive_password_message(promptPath));
        }

        throw std::runtime_error("failed to extract archive: " + promptPath.string());
    }
}

}  // namespace archive_resolver_internal
