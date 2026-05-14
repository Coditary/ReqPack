#include "rq_package_internal.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>

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

}  // namespace

namespace rq_package_internal {

RqPackageLayout load_package_layout_impl(
    const std::filesystem::path& packagePath,
    const std::filesystem::path& workRoot,
    const std::filesystem::path& stateRoot,
    const ReqPackConfig& config,
    const bool validateHostCompatibility
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

    if (validateHostCompatibility) {
        if (!rq_architecture_matches(layout.metadata.architecture, rq_host_architecture())) {
            throw std::runtime_error("package architecture does not match host");
        }

        const std::shared_ptr<const HostInfoSnapshot> hostSnapshot = HostInfoService::currentSnapshot();
        const std::set<std::string> hostSystems = rq_host_system_tokens(*hostSnapshot);
        const auto aliases = rq_merged_system_aliases(config);
        if (!rq_system_matches(layout.metadata.systems, hostSystems, aliases)) {
            throw std::runtime_error(
                "package system does not match host\npackage systems: " + rq_join_systems(layout.metadata.systems) +
                "\nhost systems: " + rq_join_systems(std::vector<std::string>(hostSystems.begin(), hostSystems.end()))
            );
        }
    }

    if (!layout.hooks.contains("install")) {
        throw std::runtime_error("rqp missing install hook");
    }
    validate_hook_files(layout.hooks, controlDir);

    if (layout.metadata.payload.has_value()) {
        const RqPayloadMetadata& payload = layout.metadata.payload.value();
        validate_payload_metadata_shape(payload);
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
        (void)validate_payload_archive_bytes(payloadArchiveBytes.value());
        extract_tar_to_directory(zstd_decompress(payloadArchiveBytes.value()), payloadDir);
        layout.hasPayload = true;
    } else if (payloadArchiveBytes.has_value() || payloadHashContent.has_value()) {
        throw std::runtime_error("payload files present but metadata.payload missing");
    }

    return layout;
}

}  // namespace rq_package_internal
