#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct ArchiveResolution {
    std::filesystem::path installPath;
    std::vector<std::filesystem::path> cleanupPaths;
    bool changed{false};
};

struct ArchiveExtractionOptions {
    std::string password{};
    bool interactive{false};
};

std::string generic_archive_suffix(const std::filesystem::path& path);
std::string archive_wrapper_suffix(const std::filesystem::path& path);
bool is_generic_archive_path(const std::filesystem::path& path);
bool is_archive_wrapper_path(const std::filesystem::path& path);
ArchiveResolution extract_archive_to_temp_directory(const std::filesystem::path& path, const ArchiveExtractionOptions& options = {});
bool extract_archive_in_place(const std::filesystem::path& path, const ArchiveExtractionOptions& options = {});
