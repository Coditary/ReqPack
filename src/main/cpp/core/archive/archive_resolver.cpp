#include "core/archive/archive_resolver.h"

#include "archive_resolver_internal.h"

#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

using archive_resolver_internal::process_archive_layers;

namespace {

void remove_cleanup_paths_quietly(const std::vector<std::filesystem::path>& cleanupPaths) {
    for (const std::filesystem::path& cleanupPath : cleanupPaths) {
        std::error_code cleanupError;
        std::filesystem::remove_all(cleanupPath, cleanupError);
    }
}

}  // namespace

std::string generic_archive_suffix(const std::filesystem::path& path) {
    static const std::array<std::string, 14> suffixes{
        ".pkg.tar.zst", ".pkg.tar.xz", ".pkg.tar.gz", ".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tgz", ".tbz2", ".txz", ".tzst", ".zip", ".7z", ".tar"
    };
    static const std::array<std::string, 4> compressedSuffixes{".gz", ".bz2", ".xz", ".zst"};

    const std::string filename = archive_resolver_internal::to_lower_copy(path.filename().string());
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
    const std::string filename = archive_resolver_internal::to_lower_copy(path.filename().string());
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

    const archive_resolver_internal::ProcessedArchivePath processed = process_archive_layers(
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
    const archive_resolver_internal::ProcessedArchivePath processed = process_archive_layers(path, options, cleanupPaths, parent);
    if (!processed.changed || processed.path == path) {
        remove_cleanup_paths_quietly(cleanupPaths);
        return false;
    }

    std::error_code error;
    std::filesystem::remove(path, error);
    if (error) {
        remove_cleanup_paths_quietly(cleanupPaths);
        throw std::runtime_error("failed to replace archive with extracted directory: " + path.string());
    }

    std::filesystem::rename(processed.path, path, error);
    if (error) {
        remove_cleanup_paths_quietly(cleanupPaths);
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
