#include "rq_package_internal.h"

#include <openssl/sha.h>

#include <zstd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace {

constexpr std::size_t TAR_BLOCK_SIZE = 512;

std::uint64_t parse_tar_octal(const char* field, std::size_t length) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < length && field[index] != '\0'; ++index) {
        const unsigned char ch = static_cast<unsigned char>(field[index]);
        if (ch == ' ' || ch == '\t') {
            continue;
        }
        if (ch < '0' || ch > '7') {
            break;
        }
        value = (value * 8) + static_cast<std::uint64_t>(ch - '0');
    }
    return value;
}

std::string parse_tar_string(const char* field, std::size_t length) {
    std::size_t size = 0;
    while (size < length && field[size] != '\0') {
        ++size;
    }
    return std::string(field, size);
}

bool tar_block_is_zero(const char* block) {
    for (std::size_t index = 0; index < TAR_BLOCK_SIZE; ++index) {
        if (block[index] != '\0') {
            return false;
        }
    }
    return true;
}

std::string normalize_tar_entry_path(std::string path) {
    while (path.rfind("./", 0) == 0) {
        path.erase(0, 2);
    }

    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }

    if (path == ".") {
        return {};
    }

    return path;
}

void validate_payload_symlink_target(const std::string& path, const std::string& linkTarget) {
    if (linkTarget.empty()) {
        throw std::runtime_error("invalid payload symlink target: " + path);
    }
    if (std::filesystem::path(linkTarget).is_absolute()) {
        throw std::runtime_error("invalid payload symlink target: " + path);
    }
}

void validate_payload_entry(const rq_package_internal::TarEntry& entry) {
    if (entry.type != '0' && entry.type != '\0' && entry.type != '5' && entry.type != '2') {
        throw std::runtime_error("unsupported payload tar entry type");
    }

    const std::filesystem::path relativePath(entry.path);
    if (rq_package_internal::path_has_invalid_segments(relativePath)) {
        throw std::runtime_error("invalid payload path: " + entry.path);
    }

    if (entry.type == '2') {
        validate_payload_symlink_target(entry.path, entry.linkTarget);
    }
}

std::uint64_t payload_installed_size(const std::vector<rq_package_internal::TarEntry>& entries) {
    std::uint64_t installedSize = 0;
    for (const rq_package_internal::TarEntry& entry : entries) {
        if (entry.type == '0' || entry.type == '\0') {
            installedSize += entry.size;
        }
    }
    return installedSize;
}

void write_tar_octal(char* field, const std::size_t length, const std::uint64_t value) {
    std::memset(field, '0', length);
    std::ostringstream stream;
    stream << std::oct << value;
    const std::string octal = stream.str();
    if (octal.size() + 1 > length) {
        throw std::runtime_error("tar field overflow");
    }
    const std::size_t offset = length - octal.size() - 1;
    std::memcpy(field + offset, octal.data(), octal.size());
    field[length - 1] = '\0';
}

void write_tar_checksum(char* field, const std::size_t length, const unsigned int value) {
    std::memset(field, ' ', length);
    std::ostringstream stream;
    stream << std::oct << value;
    const std::string octal = stream.str();
    if (octal.size() + 2 > length) {
        throw std::runtime_error("tar checksum overflow");
    }
    const std::size_t offset = length - octal.size() - 2;
    std::memcpy(field + offset, octal.data(), octal.size());
    field[length - 2] = '\0';
    field[length - 1] = ' ';
}

struct TarPathFields {
    std::string name;
    std::string prefix;
};

TarPathFields split_tar_path_fields(const std::string& path) {
    if (path.size() <= 100) {
        return TarPathFields{.name = path};
    }
    if (path.size() > 255) {
        throw std::runtime_error("archive path too long: " + path);
    }

    std::size_t separator = path.rfind('/');
    while (separator != std::string::npos) {
        const std::string prefix = path.substr(0, separator);
        const std::string name = path.substr(separator + 1);
        if (!prefix.empty() && prefix.size() <= 155 && !name.empty() && name.size() <= 100) {
            return TarPathFields{.name = name, .prefix = prefix};
        }
        if (separator == 0) {
            break;
        }
        separator = path.rfind('/', separator - 1);
    }

    throw std::runtime_error("archive path too long: " + path);
}

std::array<char, TAR_BLOCK_SIZE> tar_header_for_entry(const rq_package_internal::TarWriteEntry& entry) {
    const TarPathFields pathFields = split_tar_path_fields(entry.path);
    if (entry.type == '2' && entry.linkTarget.size() > 100) {
        throw std::runtime_error("payload symlink target too long: " + entry.path);
    }

    std::array<char, TAR_BLOCK_SIZE> header{};
    std::memcpy(header.data(), pathFields.name.data(), pathFields.name.size());
    write_tar_octal(header.data() + 100, 8, entry.mode);
    write_tar_octal(header.data() + 108, 8, 0);
    write_tar_octal(header.data() + 116, 8, 0);
    write_tar_octal(header.data() + 124, 12, entry.type == '0' ? static_cast<std::uint64_t>(entry.data.size()) : 0);
    write_tar_octal(header.data() + 136, 12, 0);
    std::memset(header.data() + 148, ' ', 8);
    header[156] = entry.type;
    if (entry.type == '2' && !entry.linkTarget.empty()) {
        std::memcpy(header.data() + 157, entry.linkTarget.data(), entry.linkTarget.size());
    }
    std::memcpy(header.data() + 257, "ustar", 5);
    std::memcpy(header.data() + 263, "00", 2);
    if (!pathFields.prefix.empty()) {
        std::memcpy(header.data() + 345, pathFields.prefix.data(), pathFields.prefix.size());
    }

    unsigned int checksum = 0;
    for (const unsigned char byte : header) {
        checksum += byte;
    }
    write_tar_checksum(header.data() + 148, 8, checksum);
    return header;
}

void append_tar_entry_bytes(std::string& output, const rq_package_internal::TarWriteEntry& entry) {
    const std::array<char, TAR_BLOCK_SIZE> header = tar_header_for_entry(entry);
    output.append(header.data(), header.size());
    if (entry.type == '0') {
        output += entry.data;
        const std::size_t padding = (TAR_BLOCK_SIZE - (entry.data.size() % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE;
        output.append(padding, '\0');
    }
}

std::string zstd_compress(const std::string& content) {
    std::string output(ZSTD_compressBound(content.size()), '\0');
    const std::size_t result = ZSTD_compress(output.data(), output.size(), content.data(), content.size(), 3);
    if (ZSTD_isError(result)) {
        throw std::runtime_error(std::string("zstd compress failed: ") + ZSTD_getErrorName(result));
    }
    output.resize(result);
    return output;
}

std::vector<rq_package_internal::TarWriteEntry> collect_payload_tree_entries(
    const std::filesystem::path& payloadRoot,
    std::uint64_t& installedSize
) {
    if (!rq_package_internal::is_directory_no_error(payloadRoot)) {
        throw std::runtime_error("payload directory not found: " + payloadRoot.string());
    }

    std::vector<rq_package_internal::TarWriteEntry> entries;
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(payloadRoot, error);
         it != std::filesystem::recursive_directory_iterator();
         it.increment(error)) {
        if (error) {
            throw std::runtime_error("failed to walk payload tree: " + payloadRoot.string());
        }

        const std::filesystem::path relativePath = std::filesystem::relative(it->path(), payloadRoot, error);
        if (error) {
            throw std::runtime_error("failed to resolve payload tree path: " + it->path().string());
        }
        if (relativePath.empty()) {
            continue;
        }

        if (rq_package_internal::path_has_invalid_segments(relativePath)) {
            throw std::runtime_error("invalid payload path: " + rq_package_internal::path_string(relativePath));
        }

        const std::filesystem::file_status status = it->symlink_status(error);
        if (error) {
            throw std::runtime_error("failed to inspect payload tree entry: " + it->path().string());
        }

        const std::string archivePath = rq_package_internal::path_string(relativePath);
        if (std::filesystem::is_symlink(status)) {
            const std::filesystem::path linkTarget = std::filesystem::read_symlink(it->path(), error);
            if (error) {
                throw std::runtime_error("failed to read payload symlink: " + it->path().string());
            }
            validate_payload_symlink_target(archivePath, rq_package_internal::path_string(linkTarget));
            entries.push_back(rq_package_internal::TarWriteEntry{
                .path = archivePath,
                .type = '2',
                .linkTarget = rq_package_internal::path_string(linkTarget),
                .mode = 0777,
            });
            continue;
        }
        if (std::filesystem::is_directory(status)) {
            entries.push_back(rq_package_internal::TarWriteEntry{
                .path = archivePath,
                .type = '5',
                .mode = 0755,
            });
            continue;
        }
        if (std::filesystem::is_regular_file(status)) {
            installedSize += std::filesystem::file_size(it->path(), error);
            if (error) {
                throw std::runtime_error("failed to stat payload file: " + it->path().string());
            }
            entries.push_back(rq_package_internal::TarWriteEntry{
                .path = archivePath,
                .type = '0',
                .data = rq_package_internal::read_file(it->path()),
                .mode = 0644,
            });
            continue;
        }
        throw std::runtime_error("unsupported payload tree entry type: " + it->path().string());
    }

    return entries;
}

}  // namespace

namespace rq_package_internal {

std::vector<TarEntry> parse_tar_entries(const std::string& content) {
    std::vector<TarEntry> entries;
    std::size_t offset = 0;
    std::string longPath;

    while (offset + TAR_BLOCK_SIZE <= content.size()) {
        const char* header = content.data() + offset;
        if (tar_block_is_zero(header)) {
            break;
        }

        const std::uint64_t size = parse_tar_octal(header + 124, 12);
        const char type = header[156] == '\0' ? '0' : header[156];
        const std::string linkTarget = parse_tar_string(header + 157, 100);
        std::string path = parse_tar_string(header, 100);
        const std::string prefix = parse_tar_string(header + 345, 155);
        if (!prefix.empty()) {
            path = prefix + "/" + path;
        }
        if (!longPath.empty()) {
            path = longPath;
            longPath.clear();
        }
        path = normalize_tar_entry_path(path);

        offset += TAR_BLOCK_SIZE;
        if (offset + size > content.size()) {
            throw std::runtime_error("tar entry exceeds archive size");
        }

        std::string data;
        if (size > 0) {
            data.assign(content.data() + offset, static_cast<std::size_t>(size));
        }
        const std::size_t paddedSize = static_cast<std::size_t>(((size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE) * TAR_BLOCK_SIZE);
        offset += paddedSize;

        if (type == 'L') {
            longPath = trim_copy(data);
            continue;
        }

        if (path.empty()) {
            continue;
        }

        entries.push_back(TarEntry{
            .path = path,
            .type = type,
            .size = size,
            .data = std::move(data),
            .linkTarget = linkTarget,
        });
    }

    return entries;
}

void validate_payload_metadata_shape(const RqPayloadMetadata& payload) {
    if (payload.path != "payload/payload.tar.zst") {
        throw std::runtime_error("unsupported payload path");
    }
    if (payload.archive != "tar" || payload.compression != "zstd" || payload.hashAlgorithm != "sha256" || payload.hashFile != "hashes/payload.sha256") {
        throw std::runtime_error("unsupported payload metadata values");
    }
}

std::vector<TarEntry> validate_payload_archive_bytes(const std::string& compressedBytes) {
    const std::vector<TarEntry> entries = parse_tar_entries(zstd_decompress(compressedBytes));
    for (const TarEntry& entry : entries) {
        validate_payload_entry(entry);
    }
    return entries;
}

std::string tar_bytes_from_entries(std::vector<TarWriteEntry> entries) {
    std::sort(entries.begin(), entries.end(), [](const TarWriteEntry& left, const TarWriteEntry& right) {
        if (left.path != right.path) {
            return left.path < right.path;
        }
        return left.type < right.type;
    });

    std::string output;
    for (const TarWriteEntry& entry : entries) {
        append_tar_entry_bytes(output, entry);
    }
    output.append(TAR_BLOCK_SIZE * 2, '\0');
    return output;
}

PayloadBuildArtifacts build_payload_from_prebuilt(const RqMetadata& metadata, const std::filesystem::path& projectRoot) {
    if (!metadata.payload.has_value()) {
        throw std::runtime_error("payload files present but metadata.payload missing");
    }

    validate_payload_metadata_shape(metadata.payload.value());
    const std::filesystem::path payloadArchivePath = projectRoot / "payload" / "payload.tar.zst";
    const std::filesystem::path payloadHashPath = projectRoot / "hashes" / "payload.sha256";
    if (!is_regular_file_no_error(payloadArchivePath)) {
        throw std::runtime_error("payload archive missing");
    }
    if (!is_regular_file_no_error(payloadHashPath)) {
        throw std::runtime_error("payload hash file missing");
    }

    PayloadBuildArtifacts artifacts;
    artifacts.hasPayload = true;
    artifacts.archiveBytes = read_file(payloadArchivePath);
    const std::string hashContent = read_file(payloadHashPath);
    const std::string expectedHash = load_payload_hash(hashContent);
    const std::string actualHash = sha256_hex(artifacts.archiveBytes);
    if (actualHash != expectedHash) {
        throw std::runtime_error("payload sha256 mismatch");
    }

    const std::vector<TarEntry> entries = validate_payload_archive_bytes(artifacts.archiveBytes);
    artifacts.hashContent = actualHash + "  payload/payload.tar.zst\n";
    artifacts.metadata = RqPayloadMetadata{
        .path = "payload/payload.tar.zst",
        .archive = "tar",
        .compression = "zstd",
        .hashAlgorithm = "sha256",
        .hashFile = "hashes/payload.sha256",
        .sizeCompressed = static_cast<std::uint64_t>(artifacts.archiveBytes.size()),
        .sizeInstalledExpected = payload_installed_size(entries),
    };
    return artifacts;
}

PayloadBuildArtifacts build_payload_from_tree(const std::filesystem::path& payloadRoot) {
    std::uint64_t installedSize = 0;
    const std::vector<TarWriteEntry> payloadEntries = collect_payload_tree_entries(payloadRoot, installedSize);
    const std::string payloadTar = tar_bytes_from_entries(payloadEntries);

    PayloadBuildArtifacts artifacts;
    artifacts.hasPayload = true;
    artifacts.archiveBytes = zstd_compress(payloadTar);
    const std::string hash = sha256_hex(artifacts.archiveBytes);
    artifacts.hashContent = hash + "  payload/payload.tar.zst\n";
    artifacts.metadata = RqPayloadMetadata{
        .path = "payload/payload.tar.zst",
        .archive = "tar",
        .compression = "zstd",
        .hashAlgorithm = "sha256",
        .hashFile = "hashes/payload.sha256",
        .sizeCompressed = static_cast<std::uint64_t>(artifacts.archiveBytes.size()),
        .sizeInstalledExpected = installedSize,
    };
    return artifacts;
}

std::string sha256_hex(const std::string& bytes) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest.data());

    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (unsigned char value : digest) {
        stream << std::setw(2) << static_cast<int>(value);
    }
    return stream.str();
}

std::string load_payload_hash(const std::string& hashFileContent) {
    std::istringstream input(hashFileContent);
    std::string hash;
    std::string path;
    if (!(input >> hash >> path)) {
        throw std::runtime_error("invalid payload hash file format");
    }
    if (path != "payload/payload.tar.zst") {
        throw std::runtime_error("payload hash file points to unexpected path");
    }
    if (hash.size() != 64 || !std::all_of(hash.begin(), hash.end(), [](unsigned char ch) {
            return std::isxdigit(ch) != 0;
        })) {
        throw std::runtime_error("payload hash is not valid sha256");
    }
    return to_lower_copy(hash);
}

std::string zstd_decompress(const std::string& compressed) {
    const unsigned long long frameSize = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
    if (frameSize == ZSTD_CONTENTSIZE_ERROR || frameSize == ZSTD_CONTENTSIZE_UNKNOWN) {
        throw std::runtime_error("invalid or unknown zstd payload size");
    }

    std::string output(static_cast<std::size_t>(frameSize), '\0');
    const std::size_t result = ZSTD_decompress(output.data(), output.size(), compressed.data(), compressed.size());
    if (ZSTD_isError(result)) {
        throw std::runtime_error(std::string("zstd decompress failed: ") + ZSTD_getErrorName(result));
    }
    output.resize(result);
    return output;
}

void append_control_tree_files(std::vector<TarWriteEntry>& entries, const std::filesystem::path& root, const std::filesystem::path& relativeRoot) {
    if (!exists_no_error(root)) {
        return;
    }
    if (!is_directory_no_error(root)) {
        throw std::runtime_error("reserved entry is not directory: " + root.string());
    }

    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(root, error);
         it != std::filesystem::recursive_directory_iterator();
         it.increment(error)) {
        if (error) {
            throw std::runtime_error("failed to read reserved directory: " + root.string());
        }

        const std::filesystem::path relativePath = relativeRoot / std::filesystem::relative(it->path(), root, error);
        if (error) {
            throw std::runtime_error("failed to resolve reserved path: " + it->path().string());
        }

        const std::string archivePath = path_string(relativePath);
        validate_outer_entry_path(archivePath);

        const std::filesystem::file_status status = it->symlink_status(error);
        if (error) {
            throw std::runtime_error("failed to inspect reserved path: " + it->path().string());
        }
        if (std::filesystem::is_directory(status)) {
            continue;
        }
        if (!std::filesystem::is_regular_file(status)) {
            throw std::runtime_error("unsupported reserved entry type: " + archivePath);
        }
        entries.push_back(TarWriteEntry{
            .path = archivePath,
            .type = '0',
            .data = read_file(it->path()),
            .mode = 0644,
        });
    }
}

void extract_tar_to_directory(const std::string& tarContent, const std::filesystem::path& targetRoot) {
    for (const TarEntry& entry : parse_tar_entries(tarContent)) {
        validate_payload_entry(entry);
        if (entry.type == '5') {
            const std::filesystem::path directoryPath = targetRoot / entry.path;
            std::filesystem::create_directories(directoryPath);
            continue;
        }

        if (entry.type == '2') {
            const std::filesystem::path outputPath = targetRoot / std::filesystem::path(entry.path);
            std::filesystem::create_directories(outputPath.parent_path());
            std::error_code error;
            std::filesystem::create_symlink(entry.linkTarget, outputPath, error);
            if (error) {
                throw std::runtime_error("failed to create payload symlink: " + entry.path);
            }
            continue;
        }

        if (entry.type != '0' && entry.type != '\0') {
            throw std::runtime_error("unsupported payload tar entry type");
        }

        const std::filesystem::path outputPath = targetRoot / std::filesystem::path(entry.path);
        std::filesystem::create_directories(outputPath.parent_path());
        write_file(outputPath, entry.data);
    }
}

}  // namespace rq_package_internal
