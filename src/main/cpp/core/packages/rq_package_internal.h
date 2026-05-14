#pragma once

#include "core/packages/rq_package.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rq_package_internal {

struct TarEntry {
    std::string path;
    char type{'0'};
    std::uint64_t size{0};
    std::string data;
    std::string linkTarget;
};

struct TarWriteEntry {
    std::string path;
    char type{'0'};
    std::string data;
    std::string linkTarget;
    std::uint32_t mode{0644};
};

struct PayloadBuildArtifacts {
    bool hasPayload{false};
    std::optional<RqPayloadMetadata> metadata;
    std::string archiveBytes;
    std::string hashContent;
};

std::string trim_copy(std::string value);
std::string read_file(const std::filesystem::path& path);
void write_file(const std::filesystem::path& path, const std::string& content);
std::string to_lower_copy(std::string value);
std::vector<std::string> normalize_string_values(const std::vector<std::string>& values);
bool path_has_invalid_segments(const std::filesystem::path& path);
bool exists_no_error(const std::filesystem::path& path);
bool is_regular_file_no_error(const std::filesystem::path& path);
bool is_directory_no_error(const std::filesystem::path& path);
std::filesystem::path absolute_path(const std::filesystem::path& path);
std::string path_string(const std::filesystem::path& path);
void validate_outer_entry_path(const std::string& rawPath);
void validate_hook_files(const std::map<std::string, std::string>& hooks, const std::filesystem::path& root);

RqMetadata parse_metadata_json_impl(const std::string& content);
std::string metadata_json_impl(const RqMetadata& metadata);
std::map<std::string, std::string> parse_reqpack_hooks_impl(const std::filesystem::path& reqpackLuaPath);

std::vector<TarEntry> parse_tar_entries(const std::string& content);
void validate_payload_metadata_shape(const RqPayloadMetadata& payload);
std::vector<TarEntry> validate_payload_archive_bytes(const std::string& compressedBytes);
std::string tar_bytes_from_entries(std::vector<TarWriteEntry> entries);
std::string sha256_hex(const std::string& bytes);
std::string load_payload_hash(const std::string& hashFileContent);
std::string zstd_decompress(const std::string& compressed);
void append_control_tree_files(std::vector<TarWriteEntry>& entries, const std::filesystem::path& root, const std::filesystem::path& relativeRoot);
void extract_tar_to_directory(const std::string& tarContent, const std::filesystem::path& targetRoot);
PayloadBuildArtifacts build_payload_from_prebuilt(const RqMetadata& metadata, const std::filesystem::path& projectRoot);
PayloadBuildArtifacts build_payload_from_tree(const std::filesystem::path& payloadRoot);

RqPackageLayout load_package_layout_impl(
    const std::filesystem::path& packagePath,
    const std::filesystem::path& workRoot,
    const std::filesystem::path& stateRoot,
    const ReqPackConfig& config,
    bool validateHostCompatibility
);
RqPackageBuildResult build_package_impl(const RqPackageBuildRequest& request, const ReqPackConfig& config);

}  // namespace rq_package_internal
