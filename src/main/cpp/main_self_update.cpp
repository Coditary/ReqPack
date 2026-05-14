#include "main_self_update.h"

#include "main_diagnostics.h"
#include "main_self_update_internal.h"

#include "output/logger.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string_view>

using self_update_internal::copy_directory_contents_with_mode;
using self_update_internal::create_self_update_temp_directory;
using self_update_internal::current_link_target_path;
using self_update_internal::ensure_directory;
using self_update_internal::extract_release_archive;
using self_update_internal::locate_extracted_binary;
using self_update_internal::parse_release_owner_repo;
using self_update_internal::refresh_update_registry;
using self_update_internal::remove_path_quietly;
using self_update_internal::replace_symlink_atomically;
using self_update_internal::resolve_self_update_asset;
using self_update_internal::sanitize_release_identifier;
using self_update_internal::self_update_download_failure_details;
using self_update_internal::self_update_missing_asset_details;
using self_update_internal::self_update_release_api_url;
using self_update_internal::self_update_release_target;

int run_self_update(const ReqPackConfig& config, Logger& logger) {
    constexpr std::string_view itemId = "rqp";
    struct SelfUpdateProgressState {
        Logger* logger{nullptr};
        std::string itemId;
        int rangeStart{0};
        int rangeEnd{100};
        int lastPercent{-1};
        std::chrono::steady_clock::time_point lastEmit{};
    };

    const auto emit_progress = [&](int percent,
                                   std::optional<std::uint64_t> currentBytes = std::nullopt,
                                   std::optional<std::uint64_t> totalBytes = std::nullopt,
                                   std::optional<std::uint64_t> bytesPerSecond = std::nullopt) {
        logger.displayItemProgress(std::string(itemId), DisplayProgressMetrics{
            .percent = percent,
            .currentBytes = currentBytes,
            .totalBytes = totalBytes,
            .bytesPerSecond = bytesPerSecond,
        });
    };

    const auto begin_step = [&](const std::string& step, int percent) {
        emit_progress(percent);
        logger.displayItemStep(std::string(itemId), step);
    };

    const auto progress_callback = [](const DownloadProgressSnapshot& snapshot, void* userData) {
        auto* state = static_cast<SelfUpdateProgressState*>(userData);
        if (state == nullptr || state->logger == nullptr) {
            return 0;
        }

        const int rawPercent = snapshot.percent.value_or(0);
        const int mappedPercent = state->rangeStart +
            static_cast<int>(((state->rangeEnd - state->rangeStart) * static_cast<long long>(rawPercent)) / 100LL);
        const auto now = std::chrono::steady_clock::now();
        if (mappedPercent < state->rangeEnd && state->lastPercent >= 0 && mappedPercent <= state->lastPercent &&
            state->lastEmit != std::chrono::steady_clock::time_point{} &&
            now - state->lastEmit < std::chrono::milliseconds(200)) {
            return 0;
        }

        state->lastPercent = mappedPercent;
        state->lastEmit = now;
        state->logger->displayItemProgress(state->itemId, DisplayProgressMetrics{
            .percent = mappedPercent,
            .currentBytes = snapshot.currentBytes,
            .totalBytes = snapshot.totalBytes,
            .bytesPerSecond = snapshot.bytesPerSecond,
        });
        return 0;
    };

    const auto fail_self_update = [&](const DiagnosticMessage& diagnostic) {
        logger.displayItemFailure(std::string(itemId), diagnostic);
        logger.displaySessionEnd(false, 0, 0, 1);
        return 1;
    };

    logger.displaySessionBegin(DisplayMode::UPDATE, {std::string(itemId)});
    logger.displayItemBegin(std::string(itemId), std::string(itemId));
    begin_step("refresh registry", 0);
    (void)refresh_update_registry(config, logger, true);
    emit_progress(5);

    if (config.selfUpdate.repoUrl.empty()) {
        return fail_self_update(self_update_diagnostic(
            "Self-update is not configured",
            "No repository URL is configured for self-update.",
            "Set selfUpdate.repoUrl in config before running `rqp update` without package arguments."
        ));
    }

    const std::optional<std::pair<std::string, std::string>> ownerRepo = parse_release_owner_repo(config.selfUpdate.repoUrl);
    if (!ownerRepo.has_value()) {
        return fail_self_update(self_update_diagnostic(
            "Self-update repository is unsupported",
            "Configured self-update repository could not be mapped to a release owner and repository name.",
            "Use repository URL that ends with `/<owner>/<repo>.git` so release API path can be derived.",
            config.selfUpdate.repoUrl
        ));
    }

    if (config.selfUpdate.binaryDirectory.empty() || config.selfUpdate.linkPath.empty()) {
        return fail_self_update(self_update_diagnostic(
            "Self-update paths are incomplete",
            "One or more required self-update paths are missing from configuration.",
            "Configure binaryDirectory and linkPath before running self-update."
        ));
    }

    const std::shared_ptr<const HostInfoSnapshot> snapshot = HostInfoService::currentSnapshot();
    const std::optional<std::string> releaseTarget = self_update_release_target(*snapshot);
    if (!releaseTarget.has_value()) {
        return fail_self_update(self_update_diagnostic(
            "Self-update target is unsupported",
            "ReqPack does not have a matching release target for this host architecture.",
            "Use a supported release target or install ReqPack manually for this platform.",
            snapshot->platform.target.empty() ? (snapshot->platform.arch + "-" + snapshot->platform.osFamily) : snapshot->platform.target
        ));
    }

    const std::optional<std::filesystem::path> tempDirectory = create_self_update_temp_directory();
    if (!tempDirectory.has_value()) {
        return fail_self_update(self_update_diagnostic(
            "Self-update workspace setup failed",
            "ReqPack could not create a temporary working directory for release download.",
            "Check temporary-directory permissions and free space, then retry self-update."
        ));
    }

    const std::filesystem::path tempPath = tempDirectory.value();
    const std::filesystem::path metadataPath = tempPath / "release.json";
    const std::filesystem::path archivePath = tempPath / "rqp.tar.gz";
    const std::filesystem::path extractPath = tempPath / "extract";
    const std::filesystem::path linkPath(config.selfUpdate.linkPath);
    const std::filesystem::path binaryDirectory(config.selfUpdate.binaryDirectory);
    const std::optional<std::filesystem::path> previousLinkTargetPath = current_link_target_path(linkPath);
    Downloader downloader(nullptr, config);
    const std::string metadataUrl = self_update_release_api_url(config.selfUpdate, ownerRepo->first, ownerRepo->second);

    const auto cleanup = [&tempPath]() {
        (void)remove_path_quietly(tempPath);
    };

    begin_step("fetch release metadata", 5);
    SelfUpdateProgressState metadataProgressState{
        .logger = &logger,
        .itemId = std::string(itemId),
        .rangeStart = 5,
        .rangeEnd = 20,
    };
    DownloadFailureDetails metadataFailureDetails;
    if (!downloader.download(
            metadataUrl,
            metadataPath.string(),
            progress_callback,
            &metadataProgressState,
            &metadataFailureDetails)) {
        cleanup();
        return fail_self_update(self_update_diagnostic(
            "Self-update metadata download failed",
            "ReqPack could not read release metadata from configured release API.",
            "Check network access, releaseApiBaseUrl, repository visibility, and selected release tag.",
            self_update_download_failure_details(metadataUrl, metadataFailureDetails)
        ));
    }
    emit_progress(20);

    const std::optional<std::pair<std::string, std::string>> asset = resolve_self_update_asset(
        ownerRepo->first,
        ownerRepo->second,
        releaseTarget.value(),
        metadataPath
    );
    if (!asset.has_value()) {
        const std::string assetDetails = self_update_missing_asset_details(config.selfUpdate, metadataPath, releaseTarget.value());
        cleanup();
        return fail_self_update(self_update_diagnostic(
            "Self-update release asset is missing",
            "Configured release does not contain a binary archive for this host target.",
            "Publish asset `rqp-<tag>-" + releaseTarget.value() + ".tar.gz` or choose a different release tag.",
            assetDetails
        ));
    }

    begin_step("download release archive", 20);
    SelfUpdateProgressState archiveProgressState{
        .logger = &logger,
        .itemId = std::string(itemId),
        .rangeStart = 20,
        .rangeEnd = 75,
    };
    DownloadFailureDetails archiveFailureDetails;
    if (!downloader.download(asset->second,
                             archivePath.string(),
                             progress_callback,
                             &archiveProgressState,
                             &archiveFailureDetails)) {
        cleanup();
        return fail_self_update(self_update_diagnostic(
            "Self-update archive download failed",
            "ReqPack found matching release metadata, but binary archive download failed.",
            "Check release asset availability and network access, then retry self-update.",
            self_update_download_failure_details(asset->second, archiveFailureDetails)
        ));
    }
    emit_progress(75);

    begin_step("extract release archive", 75);
    if (!extract_release_archive(
            archivePath,
            extractPath,
            [&](std::size_t extractedEntries, std::size_t totalEntries) {
                if (totalEntries == 0) {
                    return;
                }
                const int percent = 75 + static_cast<int>((10ULL * std::min(extractedEntries, totalEntries)) / totalEntries);
                emit_progress(percent);
            })) {
        cleanup();
        return fail_self_update(self_update_diagnostic(
            "Self-update archive extraction failed",
            "Downloaded release archive could not be extracted on this system.",
            "Ensure `tar` is available and the release asset is a valid `.tar.gz` archive.",
            archivePath.string()
        ));
    }
    emit_progress(85);

    const std::optional<std::filesystem::path> extractedBinary = locate_extracted_binary(extractPath);
    if (!extractedBinary.has_value()) {
        cleanup();
        return fail_self_update(self_update_diagnostic(
            "Self-update archive is invalid",
            "Release archive was extracted, but no `rqp` binary was found inside it.",
            "Republish release archive with `rqp` binary at archive root."
        ));
    }

    if (!ensure_directory(binaryDirectory)) {
        cleanup();
        return fail_self_update(self_update_diagnostic(
            "Self-update install directory setup failed",
            "ReqPack could not create binary install directory for downloaded release.",
            "Check write permissions for selfUpdate.binaryDirectory.",
            binaryDirectory.string()
        ));
    }

    const std::filesystem::path installedBundlePath = binaryDirectory /
        ("rqp-" + sanitize_release_identifier(asset->first) + "-" + releaseTarget.value());
    std::error_code installCleanupError;
    std::filesystem::remove_all(installedBundlePath, installCleanupError);
    if (installCleanupError) {
        cleanup();
        return fail_self_update(self_update_diagnostic(
            "Self-update install directory cleanup failed",
            "ReqPack could not prepare destination directory for downloaded release bundle.",
            "Check filesystem permissions for selfUpdate.binaryDirectory.",
            installedBundlePath.string()
        ));
    }

    begin_step("install release bundle", 85);
    if (!copy_directory_contents_with_mode(extractedBinary.value().parent_path(), installedBundlePath)) {
        cleanup();
        return fail_self_update(self_update_diagnostic(
            "Self-update bundle install failed",
            "Downloaded ReqPack release bundle could not be copied into configured self-update binary directory.",
            "Check filesystem permissions for selfUpdate.binaryDirectory.",
            installedBundlePath.string()
        ));
    }
    emit_progress(95);

    const std::filesystem::path installedBinaryPath = installedBundlePath / "rqp";
    if (!std::filesystem::exists(installedBinaryPath)) {
        cleanup();
        return fail_self_update(self_update_diagnostic(
            "Self-update bundle is invalid",
            "Downloaded release bundle was copied locally, but installed executable is missing.",
            "Republish release archive with `rqp` binary at archive root.",
            installedBundlePath.string()
        ));
    }

    begin_step("update local symlink", 95);
    if (!replace_symlink_atomically(installedBinaryPath, linkPath)) {
        cleanup();
        return fail_self_update(self_update_diagnostic(
            "Self-update symlink update failed",
            "New binary was downloaded, but ReqPack could not update configured executable symlink.",
            "Check write permissions for configured link path and parent directory.",
            config.selfUpdate.linkPath
        ));
    }
    emit_progress(98);

    cleanup();
    if (!HostInfoService::invalidateCache()) {
        logger.diagnostic(make_warning_diagnostic(
            "self-update",
            "Self-update completed, but host info cache could not be invalidated",
            "Old host metadata cache may remain until next refresh.",
            "Run `rqp host refresh` if plugins still report stale host information.",
            {},
            "self-update",
            "update"
        ));
    }

    begin_step("finalize", 99);
    if (previousLinkTargetPath.has_value() && previousLinkTargetPath.value() == installedBinaryPath) {
        logger.emit(OutputAction::DISPLAY_MESSAGE,
                    OutputContext{.message = "already on release " + asset->first,
                                  .source = std::string(itemId)});
    } else {
        logger.emit(OutputAction::DISPLAY_MESSAGE,
                    OutputContext{.message = "now on release " + asset->first,
                                  .source = std::string(itemId)});
    }
    logger.emit(OutputAction::DISPLAY_MESSAGE,
                OutputContext{.message = "link: " + config.selfUpdate.linkPath,
                              .source = std::string(itemId)});
    emit_progress(100);
    logger.displayItemSuccess(std::string(itemId));
    logger.displaySessionEnd(true, 1, 0, 0);
    return 0;
}
