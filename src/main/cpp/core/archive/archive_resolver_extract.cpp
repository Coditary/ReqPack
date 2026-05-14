#include "archive_resolver_internal.h"

#include <array>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

using archive_resolver_internal::extract_seven_zip_archive_to_directory;
using archive_resolver_internal::extract_zip_archive_to_directory;
using archive_resolver_internal::is_single_file_archive_suffix;
using archive_resolver_internal::path_has_invalid_segments;
using archive_resolver_internal::run_command_capture;
using archive_resolver_internal::run_command_capture_status;
using archive_resolver_internal::seven_zip_entries;
using archive_resolver_internal::single_file_archive_output_name;
using archive_resolver_internal::single_file_archive_output_path;
using archive_resolver_internal::split_lines;

std::string normalize_archive_entry(std::string path) {
    while (path.rfind("./", 0) == 0) {
        path.erase(0, 2);
    }
    while (!path.empty() && path.back() == '/') {
        path.pop_back();
    }
    return path;
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

std::vector<std::string> zip_archive_entries(const std::filesystem::path& archivePath) {
    const std::string archive = archive_resolver_internal::escape_shell_arg(archivePath.string());
    const std::array<std::string, 2> commands{
        "zipinfo -1 " + archive,
        "unzip -Z1 " + archive,
    };

    for (const std::string& command : commands) {
        const archive_resolver_internal::CommandResult result = run_command_capture_status(command);
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
    if (is_single_file_archive_suffix(suffix)) {
        return {single_file_archive_output_name(archivePath, suffix)};
    }
    return split_lines(run_command_capture("tar -tf " + archive_resolver_internal::escape_shell_arg(archivePath.string())));
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

    const std::string archive = archive_resolver_internal::escape_shell_arg(archivePath.string());
    const std::string output = archive_resolver_internal::escape_shell_arg(outputDirectory.string());
    const std::string singleFileOutput =
        archive_resolver_internal::escape_shell_arg(single_file_archive_output_path(archivePath, outputDirectory, suffix).string());
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
            command = "gzip -cd " + archive + " > " + singleFileOutput;
        } else if (suffix == ".bz2") {
            command = "bzip2 -cd " + archive + " > " + singleFileOutput;
        } else if (suffix == ".xz") {
            command = "xz -cd " + archive + " > " + singleFileOutput;
        } else if (suffix == ".zst") {
            command = "zstd -q -d -c " + archive + " > " + singleFileOutput;
        }

        if (command.empty() || run_command_capture_status(command).exitCode != 0) {
            throw std::runtime_error("failed to extract archive: " + promptPath.string());
        }
    }

    verify_extracted_tree(outputDirectory);
}

}  // namespace

namespace archive_resolver_internal {

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
        result.path = is_single_file_archive_suffix(archiveSuffix)
            ? single_file_archive_output_path(currentPath, outputDirectory, archiveSuffix)
            : outputDirectory;
        result.changed = true;
        return result;
    }
}

}  // namespace archive_resolver_internal
