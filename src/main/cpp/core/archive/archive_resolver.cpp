#include "core/archive/archive_resolver.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <termios.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

struct CommandResult {
    int exitCode{1};
    std::string output;
};

struct ProcessedArchivePath {
    std::filesystem::path path;
    bool changed{false};
};

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
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

std::string normalize_archive_entry(std::string path) {
    while (path.rfind("./", 0) == 0) {
        path.erase(0, 2);
    }
    while (!path.empty() && path.back() == '/') {
        path.pop_back();
    }
    return path;
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

bool output_contains(const CommandResult& result, const std::string& token) {
    return to_lower_copy(result.output).find(to_lower_copy(token)) != std::string::npos;
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

std::vector<std::string> parse_seven_zip_entries(const std::string& content) {
    std::vector<std::string> entries;
    std::istringstream input(content);
    std::string line;
    while (std::getline(input, line)) {
        line = trim_line(line);
        if (line.rfind("Path = ", 0) != 0) {
            continue;
        }

        const std::string path = line.substr(7);
        if (path.empty()) {
            continue;
        }
        if (path.find("/") == std::string::npos && path.find('\\') == std::string::npos && path == trim_line(path) &&
            path == path) {
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

std::string gpg_output_suffix(const std::filesystem::path& path) {
    const std::string wrapper = archive_wrapper_suffix(path);
    if (wrapper.empty()) {
        return ".decrypted";
    }

    const std::string filename = path.filename().string();
    if (filename.size() <= wrapper.size()) {
        return ".decrypted";
    }

    const std::string suffix = filename.substr(0, filename.size() - wrapper.size());
    return suffix.empty() ? ".decrypted" : suffix;
}

std::filesystem::path decrypt_wrapper_to_temp_file(
    const std::filesystem::path& inputPath,
    const ArchiveExtractionOptions& options,
    const std::filesystem::path& promptPath,
    std::vector<std::filesystem::path>& cleanupPaths,
    const std::filesystem::path& workingRoot
) {
    std::string password = options.password;
    bool canPrompt = options.interactive;
    const std::filesystem::path outputPath = make_unique_file_path(
        workingRoot,
        "decrypt",
        gpg_output_suffix(inputPath)
    );
    cleanupPaths.push_back(outputPath.parent_path());

    while (true) {
        const CommandResult result = run_command_capture_status(
            "gpg --batch --yes --pinentry-mode loopback --passphrase " + escape_shell_arg(password) + " -o " +
            escape_shell_arg(outputPath.string()) + " -d " + escape_shell_arg(inputPath.string())
        );
        if (result.exitCode == 0) {
            return outputPath;
        }

        if (gpg_password_error(result)) {
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

        throw std::runtime_error("failed to decrypt archive wrapper: " + promptPath.string());
    }
}

std::vector<std::string> zip_archive_entries(const std::filesystem::path& archivePath) {
    const std::string archive = escape_shell_arg(archivePath.string());
    const std::array<std::string, 2> commands{
        "zipinfo -1 " + archive,
        "unzip -Z1 " + archive,
    };

    for (const std::string& command : commands) {
        const CommandResult result = run_command_capture_status(command);
        if (result.exitCode == 0) {
            return split_lines(result.output);
        }
    }

    throw std::runtime_error("failed to inspect archive");
}

std::vector<std::string> archive_entries(
    const std::filesystem::path& archivePath,
    const std::string& suffix,
    const ArchiveExtractionOptions& options,
    const std::filesystem::path& promptPath
) {
    if (suffix == ".zip") {
        return zip_archive_entries(archivePath);
    }
    if (suffix == ".7z") {
        return seven_zip_entries(archivePath, options.password, options.interactive, promptPath);
    }
    if (suffix == ".gz" || suffix == ".bz2" || suffix == ".xz" || suffix == ".zst") {
        return {"payload"};
    }
    return split_lines(run_command_capture("tar -tf " + escape_shell_arg(archivePath.string())));
}

void validate_archive_entries(const std::vector<std::string>& entries) {
    if (entries.empty()) {
        throw std::runtime_error("archive is empty");
    }

    for (std::string entry : entries) {
        entry = normalize_archive_entry(entry);
        if (entry.empty()) {
            continue;
        }

        if (path_has_invalid_segments(std::filesystem::path(entry))) {
            throw std::runtime_error("archive contains unsafe path: " + entry);
        }
    }
}

void verify_extracted_tree(const std::filesystem::path& root) {
    std::error_code error;
    if (!std::filesystem::exists(root, error) || error) {
        throw std::runtime_error("archive extraction produced no output");
    }

    bool hasEntries = false;
    for (auto it = std::filesystem::recursive_directory_iterator(root, error);
         it != std::filesystem::recursive_directory_iterator();
         it.increment(error)) {
        if (error) {
            throw std::runtime_error("failed to inspect extracted archive");
        }

        hasEntries = true;
        const std::filesystem::directory_entry& entry = *it;
        std::error_code typeError;
        if (entry.is_symlink(typeError) || typeError) {
            throw std::runtime_error("archive contains unsupported symlink entry");
        }
    }

    if (!hasEntries) {
        throw std::runtime_error("archive extraction produced no files");
    }
}

void extract_archive_layer_to_directory(
    const std::filesystem::path& archivePath,
    const std::filesystem::path& outputDirectory,
    const ArchiveExtractionOptions& options,
    const std::filesystem::path& promptPath
) {
    const std::string suffix = generic_archive_suffix(archivePath);
    if (suffix.empty()) {
        return;
    }

    validate_archive_entries(archive_entries(archivePath, suffix, options, promptPath));

    const std::string archive = escape_shell_arg(archivePath.string());
    const std::string output = escape_shell_arg(outputDirectory.string());
    if (suffix == ".zip") {
        extract_zip_archive_to_directory(archivePath, outputDirectory, options, promptPath);
    } else if (suffix == ".7z") {
        extract_seven_zip_archive_to_directory(archivePath, outputDirectory, options, promptPath);
    } else {
        std::string command;
        if (suffix == ".tar" || suffix == ".tar.gz" || suffix == ".tgz" || suffix == ".tar.bz2" || suffix == ".tbz2" ||
            suffix == ".tar.xz" || suffix == ".txz" || suffix == ".tar.zst" || suffix == ".tzst" ||
            suffix == ".pkg.tar.gz" || suffix == ".pkg.tar.xz" || suffix == ".pkg.tar.zst") {
            command = "tar -xf " + archive + " -C " + output;
        } else if (suffix == ".gz") {
            command = "gzip -cd " + archive + " > " + output + "/payload";
        } else if (suffix == ".bz2") {
            command = "bzip2 -cd " + archive + " > " + output + "/payload";
        } else if (suffix == ".xz") {
            command = "xz -cd " + archive + " > " + output + "/payload";
        } else if (suffix == ".zst") {
            command = "zstd -q -d -c " + archive + " > " + output + "/payload";
        }

        if (command.empty() || run_command_capture_status(command).exitCode != 0) {
            throw std::runtime_error("failed to extract archive: " + promptPath.string());
        }
    }

    verify_extracted_tree(outputDirectory);
}

ProcessedArchivePath process_archive_layers(
    const std::filesystem::path& inputPath,
    const ArchiveExtractionOptions& options,
    std::vector<std::filesystem::path>& cleanupPaths,
    const std::filesystem::path& workingRoot
) {
    ProcessedArchivePath result{.path = inputPath};
    std::filesystem::path currentPath = inputPath;
    const std::filesystem::path promptPath = inputPath;

    while (true) {
        const std::string wrapper = archive_wrapper_suffix(currentPath);
        if (!wrapper.empty()) {
            currentPath = decrypt_wrapper_to_temp_file(currentPath, options, promptPath, cleanupPaths, workingRoot);
            result.changed = true;
            continue;
        }

        const std::string archiveSuffix = generic_archive_suffix(currentPath);
        if (archiveSuffix.empty()) {
            result.path = currentPath;
            return result;
        }

        const std::filesystem::path outputDirectory = make_unique_directory(workingRoot, "extract");
        cleanupPaths.push_back(outputDirectory);
        try {
            extract_archive_layer_to_directory(currentPath, outputDirectory, options, promptPath);
        } catch (...) {
            std::error_code cleanupError;
            std::filesystem::remove_all(outputDirectory, cleanupError);
            throw;
        }
        result.path = outputDirectory;
        result.changed = true;
        return result;
    }
}

}  // namespace

std::string generic_archive_suffix(const std::filesystem::path& path) {
    static const std::array<std::string, 14> suffixes{
        ".pkg.tar.zst", ".pkg.tar.xz", ".pkg.tar.gz", ".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tgz", ".tbz2", ".txz", ".tzst", ".zip", ".7z", ".tar"
    };
    static const std::array<std::string, 4> compressedSuffixes{".gz", ".bz2", ".xz", ".zst"};

    const std::string filename = to_lower_copy(path.filename().string());
    for (const std::string& suffix : suffixes) {
        if (filename.size() >= suffix.size() && filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return suffix;
        }
    }
    for (const std::string& suffix : compressedSuffixes) {
        if (filename.size() >= suffix.size() && filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return suffix;
        }
    }
    return {};
}

std::string archive_wrapper_suffix(const std::filesystem::path& path) {
    static const std::array<std::string, 2> suffixes{".gpg", ".pgp"};
    const std::string filename = to_lower_copy(path.filename().string());
    for (const std::string& suffix : suffixes) {
        if (filename.size() >= suffix.size() && filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return suffix;
        }
    }
    return {};
}

bool is_generic_archive_path(const std::filesystem::path& path) {
    return !generic_archive_suffix(path).empty();
}

bool is_archive_wrapper_path(const std::filesystem::path& path) {
    return !archive_wrapper_suffix(path).empty();
}

ArchiveResolution extract_archive_to_temp_directory(const std::filesystem::path& path, const ArchiveExtractionOptions& options) {
    ArchiveResolution result;
    result.installPath = path;
    if (!is_generic_archive_path(path) && !is_archive_wrapper_path(path)) {
        return result;
    }

    const ProcessedArchivePath processed = process_archive_layers(
        path,
        options,
        result.cleanupPaths,
        std::filesystem::temp_directory_path() / "reqpack" / "archives"
    );
    result.installPath = processed.path;
    result.changed = processed.changed;
    return result;
}

bool extract_archive_in_place(const std::filesystem::path& path, const ArchiveExtractionOptions& options) {
    if (!is_generic_archive_path(path) && !is_archive_wrapper_path(path)) {
        return false;
    }

    std::vector<std::filesystem::path> cleanupPaths;
    const std::filesystem::path parent = path.parent_path().empty() ? std::filesystem::current_path() : path.parent_path();
    const ProcessedArchivePath processed = process_archive_layers(path, options, cleanupPaths, parent);
    if (!processed.changed || processed.path == path) {
        for (const std::filesystem::path& cleanupPath : cleanupPaths) {
            std::error_code cleanupError;
            std::filesystem::remove_all(cleanupPath, cleanupError);
        }
        return false;
    }

    std::error_code error;
    std::filesystem::remove(path, error);
    if (error) {
        for (const std::filesystem::path& cleanupPath : cleanupPaths) {
            std::filesystem::remove_all(cleanupPath, error);
        }
        throw std::runtime_error("failed to replace archive with extracted directory: " + path.string());
    }

    std::filesystem::rename(processed.path, path, error);
    if (error) {
        for (const std::filesystem::path& cleanupPath : cleanupPaths) {
            std::filesystem::remove_all(cleanupPath, error);
        }
        throw std::runtime_error("failed to finalize extracted archive directory: " + path.string());
    }

    for (const std::filesystem::path& cleanupPath : cleanupPaths) {
        if (cleanupPath == processed.path) {
            continue;
        }
        std::error_code cleanupError;
        std::filesystem::remove_all(cleanupPath, cleanupError);
    }

    return true;
}
