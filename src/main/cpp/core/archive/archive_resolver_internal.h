#pragma once

#include "core/archive/archive_resolver.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace archive_resolver_internal {

struct CommandResult {
    int exitCode{1};
    std::string output;
};

struct ProcessedArchivePath {
    std::filesystem::path path;
    bool changed{false};
};

std::string to_lower_copy(std::string value);
bool is_single_file_archive_suffix(const std::string& suffix);
std::string single_file_archive_output_name(const std::filesystem::path& archivePath, const std::string& suffix);
std::filesystem::path single_file_archive_output_path(
    const std::filesystem::path& archivePath,
    const std::filesystem::path& outputDirectory,
    const std::string& suffix
);
std::filesystem::path make_unique_directory(const std::filesystem::path& root, const std::string& prefix);
std::filesystem::path make_unique_file_path(
    const std::filesystem::path& root,
    const std::string& stem,
    const std::string& suffix
);
std::string escape_shell_arg(const std::string& value);
std::string trim_line(std::string value);
std::string run_command_capture(const std::string& command);
CommandResult run_command_capture_status(const std::string& command);
bool path_has_invalid_segments(const std::filesystem::path& path);
std::vector<std::string> split_lines(const std::string& content);
bool zip_requires_password(const CommandResult& result);
bool zip_has_invalid_password(const CommandResult& result);
bool seven_zip_password_error(const CommandResult& result);
bool gpg_password_error(const CommandResult& result);
std::optional<std::string> prompt_archive_password(const std::filesystem::path& archivePath);
std::string archive_password_required_message(const std::filesystem::path& path);
std::string invalid_archive_password_message(const std::filesystem::path& path);
std::string zip_extract_command(
    const std::filesystem::path& archivePath,
    const std::filesystem::path& outputDirectory,
    const std::string& password
);
std::string seven_zip_password_flag(const std::string& password);
std::string seven_zip_extract_command(
    const std::filesystem::path& archivePath,
    const std::filesystem::path& outputDirectory,
    const std::string& password
);
std::vector<std::string> seven_zip_entries(
    const std::filesystem::path& archivePath,
    std::string password,
    bool interactive,
    const std::filesystem::path& promptPath
);
void extract_zip_archive_to_directory(
    const std::filesystem::path& archivePath,
    const std::filesystem::path& outputDirectory,
    const ArchiveExtractionOptions& options,
    const std::filesystem::path& promptPath
);
void extract_seven_zip_archive_to_directory(
    const std::filesystem::path& archivePath,
    const std::filesystem::path& outputDirectory,
    const ArchiveExtractionOptions& options,
    const std::filesystem::path& promptPath
);
std::filesystem::path decrypt_wrapper_to_temp_file(
    const std::filesystem::path& inputPath,
    const ArchiveExtractionOptions& options,
    const std::filesystem::path& promptPath,
    std::vector<std::filesystem::path>& cleanupPaths,
    const std::filesystem::path& workingRoot
);
ProcessedArchivePath process_archive_layers(
    const std::filesystem::path& inputPath,
    const ArchiveExtractionOptions& options,
    std::vector<std::filesystem::path>& cleanupPaths,
    const std::filesystem::path& workingRoot
);

}  // namespace archive_resolver_internal
