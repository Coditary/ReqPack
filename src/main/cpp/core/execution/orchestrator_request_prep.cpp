#include "orchestrator_internal.h"

#include "core/archive/archive_resolver.h"
#include "core/download/downloader.h"
#include "core/planning/request_resolution.h"
#include "output/diagnostic.h"
#include "output/logger.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {

bool is_url(const std::string& value) {
	return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0 || value.rfind("file://", 0) == 0;
}

std::string url_filename(const std::string& url) {
	const std::size_t pos = url.rfind('/');
	if (pos == std::string::npos || pos + 1 >= url.size()) {
		return "download";
	}
	const std::string raw = url.substr(pos + 1);
	const std::size_t q = raw.find('?');
	return q == std::string::npos ? raw : raw.substr(0, q);
}

std::string file_extension(const std::string& path) {
	const std::string filename = url_filename(path);
	const std::string archiveSuffix = generic_archive_suffix(filename);
	if (!archiveSuffix.empty()) {
		return archiveSuffix;
	}
	const std::size_t dot = filename.rfind('.');
	if (dot == std::string::npos || dot == 0) {
		return {};
	}
	std::string ext = filename.substr(dot);
	std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
	return ext;
}

void append_cleanup_paths(std::vector<std::filesystem::path>& tempFiles, const std::vector<std::filesystem::path>& cleanupPaths) {
	tempFiles.insert(tempFiles.end(), cleanupPaths.begin(), cleanupPaths.end());
}

ArchiveExtractionOptions archive_options_from_config(const ReqPackConfig& config) {
	return ArchiveExtractionOptions{
		.password = resolve_archive_password(config),
		.interactive = config.interaction.interactive,
	};
}

bool resolve_local_target(
	Request& request,
	Registry* registry,
	const ReqPackConfig& config,
	std::vector<std::filesystem::path>& tempFiles,
	std::string* errorMessage
) {
	try {
		const ArchiveResolution resolution = extract_archive_to_temp_directory(request.localPath, archive_options_from_config(config));
		if (resolution.changed) {
			append_cleanup_paths(tempFiles, resolution.cleanupPaths);
			request.localPath = resolution.installPath.string();
		}
	} catch (const std::exception& error) {
		if (errorMessage != nullptr) {
			*errorMessage = error.what();
		}
		return false;
	}

	if (!request.system.empty()) {
		return true;
	}

	request.system = registry->resolveSystemForLocalTarget(request.localPath);
	if (!request.system.empty()) {
		return true;
	}

	const std::string ext = file_extension(request.localPath);
	if (errorMessage != nullptr) {
		if (ext.empty()) {
			*errorMessage = "cannot determine system for local target: " + request.localPath + "\nUse: rqp install <system> <path>";
		} else {
			*errorMessage = "no plugin found for file extension '" + ext + "': " + request.localPath + "\nUse: rqp install <system> <path>";
		}
	}
	return false;
}

} // namespace

namespace orchestrator_internal {

void cleanup_temp_files(const std::vector<std::filesystem::path>& tempFiles) {
	for (const std::filesystem::path& tempFile : tempFiles) {
		std::error_code ec;
		std::filesystem::remove_all(tempFile, ec);
	}
}

bool prepare_requests_for_run(
	std::vector<Request>& requests,
	Registry* registry,
	const ReqPackConfig& config,
	std::vector<std::filesystem::path>& tempFiles
) {
	for (Request& request : requests) {
		if (request.action != ActionType::INSTALL || !request.usesLocalTarget) {
			continue;
		}

		if (!is_url(request.localPath)) {
			std::string errorMessage;
			if (!resolve_local_target(request, registry, config, tempFiles, &errorMessage)) {
				Logger::instance().diagnostic(make_error_diagnostic(
					"orchestrator",
					"Local target could not be resolved",
					"ReqPack could not infer plugin or install target details from provided local path.",
					"Specify system explicitly, for example `rqp install <system> <path>`.",
					errorMessage,
					request.system.empty() ? "install" : request.system,
					"local-target",
					{{"path", request.localPath}}
				));
				return false;
			}
			continue;
		}

		const std::string filename = url_filename(request.localPath);
		const std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "reqpack";
		std::error_code ec;
		std::filesystem::create_directories(tempDir, ec);
		const std::filesystem::path tempFile = tempDir / filename;

		Logger::instance().stdout("downloading " + request.localPath, request.system, "install");

		Downloader downloader(registry->getDatabase(), config);
		if (!downloader.download(request.localPath, tempFile.string())) {
			Logger::instance().diagnostic(make_error_diagnostic(
				"network",
				"Download failed for " + request.localPath,
				"Remote host could not be reached or returned data ReqPack could not download successfully.",
				"Check network access, verify URL, and confirm proxy or firewall settings.",
				{},
				request.system.empty() ? "download" : request.system,
				"install",
				{{"url", request.localPath}, {"target", tempFile.string()}}
			));
			return false;
		}

		tempFiles.push_back(tempFile);
		request.localPath = tempFile.string();
		std::string errorMessage;
		if (!resolve_local_target(request, registry, config, tempFiles, &errorMessage)) {
			Logger::instance().diagnostic(make_error_diagnostic(
				"orchestrator",
				"Downloaded target could not be resolved",
				"ReqPack downloaded file successfully, but could not map it to a plugin or install target.",
				"Specify system explicitly when installing remote files.",
				errorMessage,
				request.system.empty() ? "install" : request.system,
				"local-target",
				{{"path", request.localPath}}
			));
			return false;
		}
	}

	RequestResolutionService requestResolver(registry, config);
	std::string resolutionError;
	const std::optional<std::vector<Request>> resolvedRequests = requestResolver.resolveRequests(requests, &resolutionError);
	if (!resolvedRequests.has_value()) {
		if (!resolutionError.empty()) {
			Logger::instance().diagnostic(make_error_diagnostic(
				"orchestrator",
				"Request resolution failed",
				"ReqPack could not normalize one or more user requests into executable operations.",
				"Check package specifiers, system names, and command flags, then retry.",
				resolutionError,
				"resolver",
				"request"
			));
		}
		return false;
	}

	requests = resolvedRequests.value();
	return true;
}

} // namespace orchestrator_internal
