#include "core/rq_package.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <openssl/sha.h>

#include <sol/sol.hpp>

#include <zstd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace {

using boost::property_tree::ptree;

constexpr std::size_t TAR_BLOCK_SIZE = 512;

struct TarEntry {
    std::string path;
    char type{'0'};
    std::uint64_t size{0};
    std::string data;
};

std::string trim(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    return value;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("failed to open file: " + path.string());
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("failed to write file: " + path.string());
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::optional<ptree> parse_json_tree(const std::string& json) {
    if (json.empty()) {
        return std::nullopt;
    }

    std::istringstream input(json);
    ptree tree;
    try {
        boost::property_tree::read_json(input, tree);
        return tree;
    } catch (...) {
        return std::nullopt;
    }
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
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
            longPath = trim(data);
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
        });
    }

    return entries;
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
    return to_lower(hash);
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

void validate_outer_entry_path(const std::string& rawPath) {
    const std::filesystem::path path(rawPath);
    if (path_has_invalid_segments(path)) {
        throw std::runtime_error("invalid archive path: " + rawPath);
    }

    const std::string topLevel = (*path.begin()).string();
    static const std::set<std::string> allowedTopLevels{
        "metadata.json",
        "reqpack.lua",
        "hashes",
        "scripts",
        "payload",
    };

    if (!allowedTopLevels.contains(topLevel)) {
        throw std::runtime_error("unexpected top-level archive entry: " + rawPath);
    }
}

int required_int(const ptree& tree, const std::string& key) {
    const auto value = tree.get_optional<int>(key);
    if (!value.has_value()) {
        throw std::runtime_error("metadata missing integer field: " + key);
    }
    return value.value();
}

std::string required_string(const ptree& tree, const std::string& key) {
    const auto value = tree.get_optional<std::string>(key);
    if (!value.has_value() || value->empty()) {
        throw std::runtime_error("metadata missing string field: " + key);
    }
    return value.value();
}

std::vector<std::string> load_string_array(const boost::optional<const ptree&>& values) {
    std::vector<std::string> result;
    if (!values.has_value()) {
        return result;
    }
    for (const auto& [_, child] : values.value()) {
        result.push_back(child.get_value<std::string>());
    }
    return result;
}

std::vector<RqBinaryEntry> load_binaries(const boost::optional<const ptree&>& values) {
    std::vector<RqBinaryEntry> result;
    if (!values.has_value()) {
        return result;
    }
    for (const auto& [_, child] : values.value()) {
        result.push_back(RqBinaryEntry{
            .name = child.get<std::string>("name", {}),
            .installPath = child.get<std::string>("installPath", {}),
            .primary = child.get<bool>("primary", false),
        });
    }
    return result;
}

}  // namespace

RqMetadata rq_parse_metadata_json(const std::string& content) {
    const std::optional<ptree> parsed = parse_json_tree(content);
    if (!parsed.has_value()) {
        throw std::runtime_error("invalid metadata json");
    }

    const ptree& tree = parsed.value();
    RqMetadata metadata;
    metadata.formatVersion = required_int(tree, "formatVersion");
    metadata.name = required_string(tree, "name");
    metadata.version = required_string(tree, "version");
    metadata.release = required_int(tree, "release");
    metadata.revision = required_int(tree, "revision");
    metadata.summary = required_string(tree, "summary");
    metadata.description = required_string(tree, "description");
    metadata.license = required_string(tree, "license");
    metadata.architecture = required_string(tree, "architecture");
    metadata.vendor = required_string(tree, "vendor");
    metadata.maintainerEmail = required_string(tree, "maintainerEmail");
    metadata.tags = load_string_array(tree.get_child_optional("tags"));
    metadata.url = required_string(tree, "url");
    metadata.homepage = tree.get<std::string>("homepage", {});
    metadata.sourceUrl = tree.get<std::string>("sourceUrl", {});
    metadata.packager = tree.get<std::string>("packager", {});
    metadata.buildDate = tree.get<std::string>("buildDate", {});
    metadata.binaries = load_binaries(tree.get_child_optional("binaries"));
    metadata.depends = load_string_array(tree.get_child_optional("depends"));
    metadata.provides = load_string_array(tree.get_child_optional("provides"));
    metadata.conflicts = load_string_array(tree.get_child_optional("conflicts"));
    metadata.replaces = load_string_array(tree.get_child_optional("replaces"));

    if (metadata.formatVersion != 1) {
        throw std::runtime_error("unsupported rqp format version");
    }

    if (const auto payloadNode = tree.get_child_optional("payload")) {
        RqPayloadMetadata payload;
        payload.path = required_string(payloadNode.value(), "path");
        payload.archive = required_string(payloadNode.value(), "archive");
        payload.compression = required_string(payloadNode.value(), "compression");
        payload.hashAlgorithm = required_string(payloadNode.value(), "hashAlgorithm");
        payload.hashFile = required_string(payloadNode.value(), "hashFile");
        payload.sizeCompressed = payloadNode->get<std::uint64_t>("sizeCompressed", 0);
        payload.sizeInstalledExpected = payloadNode->get<std::uint64_t>("sizeInstalledExpected", 0);
        metadata.payload = payload;
    }

    return metadata;
}

std::map<std::string, std::string> rq_parse_reqpack_hooks(const std::filesystem::path& reqpackLuaPath) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::package, sol::lib::table, sol::lib::string, sol::lib::math);

    sol::load_result loadResult = lua.load_file(reqpackLuaPath.string());
    if (!loadResult.valid()) {
        const sol::error err = loadResult;
        throw std::runtime_error("failed to parse reqpack.lua: " + std::string(err.what()));
    }

    const sol::protected_function_result execResult = loadResult();
    if (!execResult.valid()) {
        const sol::error err = execResult;
        throw std::runtime_error("failed to execute reqpack.lua: " + std::string(err.what()));
    }

    sol::table packageTable;
    if (execResult.return_count() > 0 && execResult.get_type() == sol::type::table) {
        packageTable = execResult;
    } else {
        const sol::object packageObject = lua["package"];
        if (packageObject.get_type() == sol::type::table) {
            packageTable = packageObject.as<sol::table>();
        }
    }

    if (!packageTable.valid()) {
        throw std::runtime_error("reqpack.lua has no package table");
    }

    const sol::optional<int> apiVersion = packageTable["apiVersion"];
    if (!apiVersion.has_value() || apiVersion.value() != 1) {
        throw std::runtime_error("reqpack.lua apiVersion missing or unsupported");
    }

    std::map<std::string, std::string> hooks;
    const sol::object hooksObject = packageTable["hooks"];
    if (hooksObject.valid() && hooksObject.get_type() == sol::type::table) {
        const sol::table hookTable = hooksObject.as<sol::table>();
        for (const auto& [key, value] : hookTable) {
            if (key.get_type() != sol::type::string || value.get_type() != sol::type::string) {
                continue;
            }
            hooks[key.as<std::string>()] = value.as<std::string>();
        }
    }

    if (!hooks.contains("install")) {
        throw std::runtime_error("reqpack.lua missing hooks.install");
    }

    return hooks;
}

namespace {

std::filesystem::path make_unique_directory(const std::filesystem::path& parent, const std::string& prefix) {
    std::filesystem::create_directories(parent);
    std::filesystem::path pattern = parent / (prefix + "-XXXXXX");
    std::string value = pattern.string();
    std::vector<char> buffer(value.begin(), value.end());
    buffer.push_back('\0');
    char* created = ::mkdtemp(buffer.data());
    if (created == nullptr) {
        throw std::runtime_error("failed to create temporary directory");
    }
    return std::filesystem::path(created);
}

void extract_tar_to_directory(const std::string& tarContent, const std::filesystem::path& targetRoot) {
    for (const TarEntry& entry : parse_tar_entries(tarContent)) {
        if (entry.type == '5') {
            const std::filesystem::path directoryPath = targetRoot / entry.path;
            if (path_has_invalid_segments(std::filesystem::path(entry.path))) {
                throw std::runtime_error("invalid payload path: " + entry.path);
            }
            std::filesystem::create_directories(directoryPath);
            continue;
        }

        if (entry.type != '0' && entry.type != '\0') {
            throw std::runtime_error("unsupported payload tar entry type");
        }

        const std::filesystem::path relativePath(entry.path);
        if (path_has_invalid_segments(relativePath)) {
            throw std::runtime_error("invalid payload path: " + entry.path);
        }

        const std::filesystem::path outputPath = targetRoot / relativePath;
        std::filesystem::create_directories(outputPath.parent_path());
        write_file(outputPath, entry.data);
    }
}

}  // namespace

std::string rq_host_architecture() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__)
    return "aarch64";
#elif defined(__arm__)
    return "armv7";
#else
    return "unknown";
#endif
}

std::string rq_package_identity(const RqMetadata& metadata) {
    return metadata.name + "@" + metadata.version + "-" + std::to_string(metadata.release) + "+r" + std::to_string(metadata.revision);
}

bool rq_architecture_matches(const std::string& packageArchitecture, const std::string& hostArchitecture) {
    return packageArchitecture == "noarch" || packageArchitecture == hostArchitecture;
}

RqPackageLayout RqPackageReader::load(
    const std::filesystem::path& packagePath,
    const std::filesystem::path& workRoot,
    const std::filesystem::path& stateRoot
) {
    if (!std::filesystem::is_regular_file(packagePath)) {
        throw std::runtime_error("rqp package not found: " + packagePath.string());
    }

    const std::filesystem::path controlDir = make_unique_directory(workRoot, "rqp-control");
    const std::filesystem::path payloadDir = make_unique_directory(workRoot, "rqp-payload");
    const std::filesystem::path workDir = make_unique_directory(workRoot, "rqp-work");

    std::optional<std::string> metadataContent;
    std::optional<std::string> reqpackContent;
    std::optional<std::string> payloadHashContent;
    std::optional<std::string> payloadArchiveBytes;
    for (const TarEntry& entry : parse_tar_entries(read_file(packagePath))) {
        validate_outer_entry_path(entry.path);
        if (entry.type != '0' && entry.type != '\0' && entry.type != '5') {
            throw std::runtime_error("unsupported outer archive entry type");
        }
        if (entry.type == '5') {
            continue;
        }

        const std::filesystem::path relativePath(entry.path);
        const std::filesystem::path targetPath = controlDir / relativePath;
        write_file(targetPath, entry.data);

        if (entry.path == "metadata.json") {
            metadataContent = entry.data;
        } else if (entry.path == "reqpack.lua") {
            reqpackContent = entry.data;
        } else if (entry.path == "hashes/payload.sha256") {
            payloadHashContent = entry.data;
        } else if (entry.path == "payload/payload.tar.zst") {
            payloadArchiveBytes = entry.data;
        }
    }

    if (!metadataContent.has_value()) {
        throw std::runtime_error("rqp missing metadata.json");
    }
    if (!reqpackContent.has_value()) {
        throw std::runtime_error("rqp missing reqpack.lua");
    }

    RqPackageLayout layout;
    layout.packagePath = packagePath;
    layout.controlDir = controlDir;
    layout.payloadDir = payloadDir;
    layout.workDir = workDir;
    layout.metadata = rq_parse_metadata_json(metadataContent.value());
    layout.identity = rq_package_identity(layout.metadata);
    layout.stateDir = stateRoot / layout.metadata.name / layout.identity;
    layout.hooks = rq_parse_reqpack_hooks(controlDir / "reqpack.lua");

    if (!rq_architecture_matches(layout.metadata.architecture, rq_host_architecture())) {
        throw std::runtime_error("package architecture does not match host");
    }

    if (!layout.hooks.contains("install")) {
        throw std::runtime_error("rqp missing install hook");
    }

    const std::filesystem::path installHookPath = controlDir / layout.hooks.at("install");
    if (!std::filesystem::is_regular_file(installHookPath)) {
        throw std::runtime_error("install hook file not found");
    }

    if (layout.metadata.payload.has_value()) {
        const RqPayloadMetadata& payload = layout.metadata.payload.value();
        if (payload.path != "payload/payload.tar.zst") {
            throw std::runtime_error("unsupported payload path");
        }
        if (payload.archive != "tar" || payload.compression != "zstd" || payload.hashAlgorithm != "sha256" || payload.hashFile != "hashes/payload.sha256") {
            throw std::runtime_error("unsupported payload metadata values");
        }
        if (!payloadArchiveBytes.has_value()) {
            throw std::runtime_error("payload archive missing");
        }
        if (!payloadHashContent.has_value()) {
            throw std::runtime_error("payload hash file missing");
        }

        const std::string expectedHash = load_payload_hash(payloadHashContent.value());
        const std::string actualHash = sha256_hex(payloadArchiveBytes.value());
        if (actualHash != expectedHash) {
            throw std::runtime_error("payload sha256 mismatch");
        }

        layout.payloadArchivePath = controlDir / "payload" / "payload.tar.zst";
        extract_tar_to_directory(zstd_decompress(payloadArchiveBytes.value()), payloadDir);
        layout.hasPayload = true;
    } else if (payloadArchiveBytes.has_value() || payloadHashContent.has_value()) {
        throw std::runtime_error("payload files present but metadata.payload missing");
    }

    return layout;
}
