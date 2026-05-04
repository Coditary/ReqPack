#include "core/orchestrator.h"

#include "core/archive_resolver.h"
#include "core/downloader.h"
#include "core/planner_core.h"
#include "core/request_resolution.h"
#include "output/command_output.h"
#include "output/logger.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <utility>

namespace {

bool is_url(const std::string& value) {
    return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0 || value.rfind("file://", 0) == 0;
}

std::string package_specifier_from_info(const PackageInfo& item) {
    if (item.version.empty()) {
        return item.name;
    }
    return item.name + '@' + item.version;
}

bool package_info_has_details(const PackageInfo& item) {
	return !item.packageId.empty() || !item.version.empty() || !item.latestVersion.empty() ||
		!item.status.empty() || !item.installed.empty() || !item.summary.empty() || !item.description.empty() ||
		!item.homepage.empty() || !item.documentation.empty() || !item.sourceUrl.empty() || !item.repository.empty() ||
		!item.channel.empty() || !item.section.empty() || !item.packageType.empty() || !item.architecture.empty() || !item.license.empty() ||
		!item.author.empty() || !item.maintainer.empty() || !item.email.empty() || !item.publishedAt.empty() ||
		!item.updatedAt.empty() || !item.size.empty() || !item.installedSize.empty() ||
		!item.dependencies.empty() || !item.optionalDependencies.empty() || !item.provides.empty() ||
		!item.conflicts.empty() || !item.replaces.empty() || !item.binaries.empty() || !item.tags.empty() ||
		!item.extraFields.empty();
}

CommandOutput package_table_output(ActionType action,
	                               const std::vector<Request>& requests,
	                               const std::vector<PackageInfo>& items) {
	CommandOutput output;
	output.mode = action == ActionType::LIST ? DisplayMode::LIST
	              : action == ActionType::SEARCH ? DisplayMode::SEARCH
	              : action == ActionType::OUTDATED ? DisplayMode::OUTDATED
	              : DisplayMode::LIST;
	for (const auto& request : requests) {
		output.sessionItems.push_back(request.system.empty() ? "all" : request.system);
	}
	const bool includeSystem = requests.size() > 1;
	std::vector<std::string> headers;
	const bool searchTable = action == ActionType::SEARCH;
	if (includeSystem) {
		headers.push_back("System");
	}
	headers.push_back("Name");
	headers.push_back("Version");
	if (searchTable) {
		headers.push_back("Type");
		headers.push_back("Architecture");
		headers.push_back("Description");
		output.blocks.push_back(make_command_table_block(headers, package_search_infos_to_rows(items, includeSystem)));
	} else {
		headers.push_back("Summary");
		output.blocks.push_back(make_command_table_block(headers, package_infos_to_rows(items, includeSystem)));
	}
	if (items.empty()) {
		output.blocks.push_back(make_command_message_block("No results"));
	}
	output.success = true;
	output.succeeded = static_cast<int>(items.size());
	return output;
}

CommandOutput package_info_output(const Request& request, PackageInfo item) {
	CommandOutput output;
	output.mode = DisplayMode::INFO;
	output.sessionItems = {request.system.empty() ? "info" : request.system};
	if (!package_info_has_details(item)) {
		output.success = false;
		output.failed = 1;
		output.blocks.push_back(make_command_message_block("No package info found"));
		return output;
	}
	if (item.system.empty()) {
		item.system = request.system;
	}
	if (item.name.empty() && !request.packages.empty()) {
		item.name = request.packages.front();
	}
	const auto fields = package_info_to_fields(item);
	output.blocks.push_back(make_command_field_value_block(fields));
	output.success = true;
	output.succeeded = 1;
	return output;
}

// Extract the filename from a URL path (everything after the last '/').
std::string url_filename(const std::string& url) {
    const std::size_t pos = url.rfind('/');
    if (pos == std::string::npos || pos + 1 >= url.size()) {
        return "download";
    }
    // Strip any query string.
    const std::string raw = url.substr(pos + 1);
    const std::size_t q = raw.find('?');
    return q == std::string::npos ? raw : raw.substr(0, q);
}

// Extract the lowercase file extension from a filename/URL.
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

bool resolve_local_target(Request& request, Registry* registry, std::vector<std::filesystem::path>& tempFiles, std::string* errorMessage) {
    try {
        const ArchiveResolution resolution = extract_archive_to_temp_directory(request.localPath);
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
            *errorMessage = "cannot determine system for local target: " + request.localPath + "\nUse: ReqPack install <system> <path>";
        } else {
            *errorMessage = "no plugin found for file extension '" + ext + "': " + request.localPath + "\nUse: ReqPack install <system> <path>";
        }
    }
    return false;
}

void log_validation_blocked(const std::vector<ValidationFinding>& findings) {
    Logger::instance().err("execution blocked by security policy");
    std::size_t shown = 0;
    for (const ValidationFinding& finding : findings) {
        if (finding.kind != "vulnerability" && finding.kind != "sync_error" && finding.kind != "unsupported_ecosystem" &&
            finding.kind != "unresolved_version") {
            continue;
        }

        std::string message = finding.message.empty() ? finding.id : finding.message;
        if (!finding.package.system.empty() && !finding.package.name.empty()) {
            message += " [" + finding.package.system + ":" + finding.package.name + "]";
        }
        Logger::instance().err(message);
        ++shown;
        if (shown >= 5) {
            break;
        }
    }
}

bool has_explicit_version(const std::string& packageSpecifier) {
    const std::size_t versionSeparator = packageSpecifier.rfind('@');
    return versionSeparator != std::string::npos && versionSeparator != 0 && versionSeparator + 1 < packageSpecifier.size();
}

struct SbomResolutionResult {
    std::vector<Request> requests;
    std::vector<std::string> missingPackages;
};

std::vector<Request> expand_system_only_audit_requests(Executer* executor, const std::vector<Request>& requests) {
    std::vector<Request> expanded = requests;
    if (executor == nullptr) {
        return expanded;
    }

    for (Request& request : expanded) {
        if (request.action != ActionType::AUDIT || request.system.empty() || request.usesLocalTarget || !request.packages.empty()) {
            continue;
        }

        const std::vector<PackageInfo> installedPackages = executor->list(request);
        request.packages.clear();
        request.packages.reserve(installedPackages.size());
        for (const PackageInfo& item : installedPackages) {
            if (item.name.empty()) {
                continue;
            }
            request.packages.push_back(package_specifier_from_info(item));
        }
    }

    return expanded;
}

SbomResolutionResult resolve_sbom_requests(Executer* executor, const ReqPackConfig& config, const std::vector<Request>& requests) {
    SbomResolutionResult result{.requests = requests};
    if (executor == nullptr) {
        return result;
    }

    for (Request& request : result.requests) {
        if (request.action != ActionType::SBOM || request.system.empty() || request.usesLocalTarget || request.packages.empty()) {
            continue;
        }

        std::vector<std::string> resolvedPackages;
        resolvedPackages.reserve(request.packages.size());
        for (const std::string& packageSpecifier : request.packages) {
            const Package requestedPackage = planner_make_requested_package(request, request.system, packageSpecifier);
            const std::optional<Package> resolvedPackage = executor->resolvePackage(request, requestedPackage);
            if (resolvedPackage.has_value()) {
                resolvedPackages.push_back(planner_package_specifier_from_package(resolvedPackage.value()));
                continue;
            }

            if (has_explicit_version(packageSpecifier) || !config.sbom.skipMissingPackages) {
                result.missingPackages.push_back(request.system + ":" + packageSpecifier);
                continue;
            }

            Logger::instance().warn("sbom skipping missing package: " + request.system + ":" + packageSpecifier);
        }
        request.packages = std::move(resolvedPackages);
    }

    return result;
}

} // namespace

Orchestrator::Orchestrator(std::vector<Request> requests, const ReqPackConfig& config)
	: config(config), requests(std::move(requests)) {
	this->registry  = new Registry(this->config);
	this->planner   = new Planner(this->registry, this->registry->getDatabase(), this->config);
	this->auditExporter = new AuditExporter(this->registry, this->config);
	this->sbomExporter = new SbomExporter(this->registry, this->config);
	this->snapshotExporter = new SnapshotExporter(this->config);
	this->validator = new Validator(this->registry, this->config);
	this->executor  = new Executer(this->registry, this->config);
}

Orchestrator::~Orchestrator() {
	delete this->registry;
	delete this->planner;
	delete this->auditExporter;
	delete this->sbomExporter;
	delete this->snapshotExporter;
	delete this->validator;
	delete this->executor;
}

int Orchestrator::countRequestedItems() const {
	int count = 0;
	for (const Request& request : this->requests) {
		if (request.action != ActionType::INSTALL && request.action != ActionType::ENSURE &&
			request.action != ActionType::REMOVE && request.action != ActionType::UPDATE &&
			request.action != ActionType::LIST && request.action != ActionType::OUTDATED) {
			continue;
		}
		if (request.usesLocalTarget) {
			++count;
			continue;
		}
		count += static_cast<int>(request.packages.size());
	}
	return count;
}

int Orchestrator::run() {
	(void)this->registry->getDatabase()->ensureReady();
	this->registry->scanDirectory(this->config.registry.pluginDirectory);
	const std::filesystem::path workspacePluginDirectory = std::filesystem::current_path() / "plugins";
	const std::filesystem::path configuredPluginDirectory = std::filesystem::path(this->config.registry.pluginDirectory);
	if (std::filesystem::exists(workspacePluginDirectory) && workspacePluginDirectory != configuredPluginDirectory) {
		this->registry->scanDirectory(workspacePluginDirectory.string());
	}
	auto cleanupTempFiles = [](const std::vector<std::filesystem::path>& tempFiles) {
		for (const std::filesystem::path& tempFile : tempFiles) {
			std::error_code ec;
			std::filesystem::remove_all(tempFile, ec);
		}
	};
	if (this->requests.empty()) {
		return 0;
	}

	// ── URL pre-processing ────────────────────────────────────────────────────
	// For any INSTALL request whose localPath is a URL:
	//   1. If system is empty, resolve it from the file extension via plugin declarations.
	//   2. Download the URL to a temp file and replace localPath with the local path.
	// Temp files are tracked and deleted after execution.
	std::vector<std::filesystem::path> tempFiles;
	for (Request& request : this->requests) {
		if (request.action != ActionType::INSTALL || !request.usesLocalTarget) {
			continue;
		}

		if (!is_url(request.localPath)) {
			std::string errorMessage;
			if (!resolve_local_target(request, this->registry, tempFiles, &errorMessage)) {
				Logger::instance().err(errorMessage);
				cleanupTempFiles(tempFiles);
				return 1;
			}
			continue;
		}

		const std::string filename = url_filename(request.localPath);
		const std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "reqpack";
		std::error_code ec;
		std::filesystem::create_directories(tempDir, ec);
		const std::filesystem::path tempFile = tempDir / filename;

		Logger::instance().stdout("downloading " + request.localPath, request.system, "install");

		Downloader downloader(this->registry->getDatabase(), this->config);
		if (!downloader.download(request.localPath, tempFile.string())) {
			Logger::instance().err("failed to download: " + request.localPath);
			cleanupTempFiles(tempFiles);
			return 1;
		}

		tempFiles.push_back(tempFile);
		request.localPath = tempFile.string();
		std::string errorMessage;
		if (!resolve_local_target(request, this->registry, tempFiles, &errorMessage)) {
			Logger::instance().err(errorMessage);
			cleanupTempFiles(tempFiles);
			return 1;
		}
	}
	RequestResolutionService requestResolver(this->registry, this->config);
	std::string resolutionError;
	const std::optional<std::vector<Request>> resolvedRequests = requestResolver.resolveRequests(this->requests, &resolutionError);
	if (!resolvedRequests.has_value()) {
		if (!resolutionError.empty()) {
			Logger::instance().err(resolutionError);
		}
		cleanupTempFiles(tempFiles);
		return 1;
	}
	this->requests = resolvedRequests.value();
	// ─────────────────────────────────────────────────────────────────────────

	if (this->requests.front().action == ActionType::LIST) {
		std::vector<PackageInfo> items;
		for (const Request& request : this->requests) {
			auto listed = this->executor->list(request);
			for (auto& item : listed) {
				if (item.system.empty()) {
					item.system = request.system;
				}
				items.push_back(std::move(item));
			}
		}
		render_command_output(package_table_output(ActionType::LIST, this->requests, items));
		return 0;
	}

	if (this->requests.front().action == ActionType::OUTDATED) {
		std::vector<PackageInfo> items;
		for (const Request& request : this->requests) {
			auto outdated = this->executor->outdated(request);
			for (auto& item : outdated) {
				if (item.system.empty()) {
					item.system = request.system;
				}
				items.push_back(std::move(item));
			}
		}
		render_command_output(package_table_output(ActionType::OUTDATED, this->requests, items));
		return 0;
	}

	if (this->requests.front().action == ActionType::SNAPSHOT) {
		const bool ok = this->snapshotExporter->exportSnapshot(this->requests.front());
		return ok ? 0 : 1;
	}

	if (this->requests.front().action == ActionType::SEARCH) {
		std::vector<PackageInfo> items;
		for (const Request& request : this->requests) {
			auto searched = this->executor->search(request);
			for (auto& item : searched) {
				if (item.system.empty()) {
					item.system = request.system;
				}
				items.push_back(std::move(item));
			}
		}
		render_command_output(package_table_output(ActionType::SEARCH, this->requests, items));
		return 0;
	}

	if (this->requests.front().action == ActionType::INFO) {
		const PackageInfo item = this->executor->info(this->requests.front());
		const CommandOutput output = package_info_output(this->requests.front(), item);
		render_command_output(output);
		return output.success ? 0 : 1;
	}

	std::vector<Request> plannedRequests = this->requests;
	std::vector<std::string> missingSbomPackages;
	if (this->requests.front().action == ActionType::AUDIT) {
		plannedRequests = expand_system_only_audit_requests(this->executor, this->requests);
	} else if (this->requests.front().action == ActionType::SBOM) {
		SbomResolutionResult resolvedSbom = resolve_sbom_requests(this->executor, this->config, this->requests);
		plannedRequests = std::move(resolvedSbom.requests);
		missingSbomPackages = std::move(resolvedSbom.missingPackages);
	}
	if (this->requests.front().action == ActionType::SBOM && !missingSbomPackages.empty()) {
		for (const std::string& packageSpecifier : missingSbomPackages) {
			Logger::instance().err("sbom missing package: " + packageSpecifier);
		}
		cleanupTempFiles(tempFiles);
		return 1;
	}
	Graph* graph = this->planner->plan(plannedRequests);
	if (this->requests.front().action == ActionType::SBOM) {
		if (graph != nullptr && boost::num_vertices(*graph) > 0) {
			(void)this->sbomExporter->exportGraph(*graph, this->requests.front());
		}
		cleanupTempFiles(tempFiles);
		delete graph;
		return 0;
	}
	if (this->requests.front().action == ActionType::AUDIT) {
		const std::vector<ValidationFinding> findings = this->validator->audit(graph);
		const bool exported = graph != nullptr && this->auditExporter->exportGraph(*graph, findings, this->requests.front());
		delete graph;
		cleanupTempFiles(tempFiles);
		if (!exported) {
			return 1;
		}
		if (!this->requests.front().outputPath.empty()) {
			return 0;
		}
		return findings.empty() ? 0 : 1;
	}
	Graph* validatedGraph = this->validator->validate(graph);
	if (validatedGraph == nullptr) {
		if (graph != nullptr) {
			log_validation_blocked(this->validator->getLastFindings());
		}
		delete graph;
		cleanupTempFiles(tempFiles);
		return 1;
	}
	graph = validatedGraph;
	this->executor->setRequestedItemCount(this->countRequestedItems(), true);
	this->executor->execute(graph);
	delete graph;

	cleanupTempFiles(tempFiles);
	return 0;
}
