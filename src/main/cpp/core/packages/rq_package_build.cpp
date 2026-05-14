#include "rq_package_internal.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <system_error>

namespace rq_package_internal {

RqPackageBuildResult build_package_impl(const RqPackageBuildRequest& request, const ReqPackConfig& config) {
    const std::filesystem::path projectRoot = absolute_path(request.projectRoot);
    if (!is_directory_no_error(projectRoot)) {
        throw std::runtime_error("pack project directory not found: " + projectRoot.string());
    }

    const std::filesystem::path metadataPath = projectRoot / "metadata.json";
    const std::filesystem::path reqpackPath = projectRoot / "reqpack.lua";
    if (!is_regular_file_no_error(metadataPath)) {
        throw std::runtime_error("pack project is missing metadata.json");
    }
    if (!is_regular_file_no_error(reqpackPath)) {
        throw std::runtime_error("pack project is missing reqpack.lua");
    }

    RqMetadata metadata = rq_parse_metadata_json(read_file(metadataPath));
    metadata.architecture = rq_normalize_architecture(metadata.architecture);
    metadata.systems = rq_normalize_systems(metadata.systems);

    const std::map<std::string, std::string> hooks = rq_parse_reqpack_hooks(reqpackPath);
    validate_hook_files(hooks, projectRoot);

    const bool hasEmbeddedPayloadTree = exists_no_error(projectRoot / "payload-tree");
    const bool hasPayloadDir = exists_no_error(projectRoot / "payload");
    const bool hasHashesDir = exists_no_error(projectRoot / "hashes");
    const bool hasExternalPayload = request.payloadRoot.has_value();

    if (hasExternalPayload && hasEmbeddedPayloadTree) {
        throw std::runtime_error("pack input is ambiguous: use either payload-tree/ or --payload-dir, not both");
    }
    if (hasEmbeddedPayloadTree && (hasPayloadDir || hasHashesDir)) {
        throw std::runtime_error("pack input is ambiguous: payload-tree/ cannot be combined with payload/ or hashes/");
    }
    if (hasPayloadDir != hasHashesDir) {
        throw std::runtime_error("pack input is incomplete: payload/ and hashes/ must both be present");
    }

    PayloadBuildArtifacts payloadArtifacts;
    if (hasExternalPayload) {
        payloadArtifacts = build_payload_from_tree(absolute_path(request.payloadRoot.value()));
    } else if (hasEmbeddedPayloadTree) {
        payloadArtifacts = build_payload_from_tree(projectRoot / "payload-tree");
    } else if (hasPayloadDir && hasHashesDir) {
        payloadArtifacts = build_payload_from_prebuilt(metadata, projectRoot);
    } else {
        if (metadata.payload.has_value()) {
            throw std::runtime_error("metadata.payload requires payload files or payload tree");
        }
    }

    if (payloadArtifacts.hasPayload) {
        metadata.payload = payloadArtifacts.metadata;
    } else {
        metadata.payload.reset();
    }

    std::filesystem::path outputPath = request.outputPath;
    if (outputPath.empty()) {
        const std::filesystem::path currentRoot = absolute_path(std::filesystem::current_path());
        outputPath = (projectRoot == currentRoot ? projectRoot : projectRoot.parent_path()) / (metadata.name + ".rqp");
    }
    if (outputPath.is_relative()) {
        outputPath = std::filesystem::current_path() / outputPath;
    }

    if (exists_no_error(outputPath) && !request.force) {
        if (!request.interactive) {
            throw std::runtime_error("output file already exists: " + outputPath.string() + "\nUse --force to overwrite.");
        }
        std::cout << outputPath.string() << " already exists. Overwrite? [y/N]\n";
        std::cout.flush();
        std::string answer;
        if (!std::getline(std::cin, answer) || (answer != "y" && answer != "Y")) {
            throw std::runtime_error("pack aborted");
        }
    }

    std::vector<TarWriteEntry> packageEntries;
    packageEntries.push_back(TarWriteEntry{
        .path = "metadata.json",
        .type = '0',
        .data = rq_metadata_json(metadata),
        .mode = 0644,
    });
    packageEntries.push_back(TarWriteEntry{
        .path = "reqpack.lua",
        .type = '0',
        .data = read_file(reqpackPath),
        .mode = 0644,
    });
    append_control_tree_files(packageEntries, projectRoot / "scripts", "scripts");

    if (payloadArtifacts.hasPayload) {
        packageEntries.push_back(TarWriteEntry{
            .path = "hashes/payload.sha256",
            .type = '0',
            .data = payloadArtifacts.hashContent,
            .mode = 0644,
        });
        packageEntries.push_back(TarWriteEntry{
            .path = "payload/payload.tar.zst",
            .type = '0',
            .data = payloadArtifacts.archiveBytes,
            .mode = 0644,
        });
    }

    const std::string archiveBytes = tar_bytes_from_entries(packageEntries);
    std::error_code error;
    if (!outputPath.parent_path().empty()) {
        std::filesystem::create_directories(outputPath.parent_path(), error);
        if (error) {
            throw std::runtime_error("failed to create output directory: " + outputPath.parent_path().string());
        }
    }
    write_file(outputPath, archiveBytes);

    const std::filesystem::path validationRoot = std::filesystem::temp_directory_path() / "reqpack-rqp-pack-validate";
    (void)RqPackageReader::load(outputPath, validationRoot / "work", validationRoot / "state", config, false);

    return RqPackageBuildResult{
        .metadata = metadata,
        .identity = rq_package_identity(metadata),
        .outputPath = outputPath,
        .hasPayload = payloadArtifacts.hasPayload,
    };
}

}  // namespace rq_package_internal
