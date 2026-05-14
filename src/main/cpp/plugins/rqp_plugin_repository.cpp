#include "rqp_plugin_internal.h"

#include "core/archive/archive_resolver.h"

#include <openssl/sha.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

std::optional<std::filesystem::path> rqp_plugin_unique_nested_file_with_extension(
    const std::filesystem::path& root,
    const std::string& extension
) {
    std::optional<std::filesystem::path> match;
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(root, error);
         it != std::filesystem::recursive_directory_iterator();
         it.increment(error)) {
        if (error || !it->is_regular_file()) {
            continue;
        }
        if (it->path().extension() != extension) {
            continue;
        }
        if (match.has_value()) {
            throw std::runtime_error("multiple installable rqp files found in extracted archive");
        }
        match = it->path();
    }
    return match;
}

std::vector<RqRepositoryIndex> RqpPlugin::loadRepositoryIndexes(const PluginCallContext& context) const {
    std::vector<RqRepositoryIndex> indexes;
    indexes.reserve(config_.rqp.repositories.size());
    for (const std::string& repository : config_.rqp.repositories) {
        const std::filesystem::path path = downloadPackageArtifact(context, repository);
        if (path.empty()) {
            throw std::runtime_error("failed to load rqp repository index: " + repository);
        }
        indexes.push_back(rq_repository_parse_index(readTextFile(path), repository));
    }
    return indexes;
}

std::string RqpPlugin::readTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("failed to open file: " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string RqpPlugin::sha256Hex(const std::string& bytes) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest.data());

    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (unsigned char value : digest) {
        stream << std::setw(2) << static_cast<int>(value);
    }
    return stream.str();
}

std::filesystem::path RqpPlugin::localPathForUrl(const std::string& url) {
    static constexpr const char* filePrefix = "file://";
    if (url.rfind(filePrefix, 0) == 0) {
        return std::filesystem::path(url.substr(7));
    }
    return std::filesystem::path(url);
}

std::filesystem::path RqpPlugin::downloadPackageArtifact(const PluginCallContext& context, const std::string& url) {
    const std::filesystem::path localPath = localPathForUrl(url);
    if (localPath != std::filesystem::path(url) && !is_generic_archive_path(localPath) && !is_archive_wrapper_path(localPath)) {
        return localPath;
    }

    const std::string tempDirectory = context.createTempDirectory();
    if (tempDirectory.empty()) {
        return {};
    }
    const std::filesystem::path sourcePath = (localPath != std::filesystem::path(url)) ? localPath : std::filesystem::path(url);
    const std::string suffix = generic_archive_suffix(sourcePath);
    const std::string wrapper = archive_wrapper_suffix(sourcePath);
    std::string extension;
    if (!wrapper.empty()) {
        const std::string innerSuffix = generic_archive_suffix(sourcePath.stem());
        extension = innerSuffix.empty() ? wrapper : innerSuffix + wrapper;
    } else {
        extension = suffix.empty() ? sourcePath.extension().string() : suffix;
    }
    const std::filesystem::path targetPath = std::filesystem::path(tempDirectory) / (extension.empty() ? "download.rqp" : "download" + extension);
    const DownloadResult download = context.downloadFile(url, targetPath.string());
    if (!download.success) {
        return {};
    }

    std::filesystem::path resolvedPath = download.resolvedPath.empty() ? targetPath : std::filesystem::path(download.resolvedPath);
    if (std::filesystem::is_directory(resolvedPath)) {
        const std::filesystem::path nestedPackage = resolvedPath / "download.rqp";
        if (std::filesystem::is_regular_file(nestedPackage)) {
            return nestedPackage;
        }

        const std::optional<std::filesystem::path> discoveredPackage = rqp_plugin_unique_nested_file_with_extension(resolvedPath, ".rqp");
        if (discoveredPackage.has_value()) {
            return discoveredPackage.value();
        }
    }
    return resolvedPath;
}
