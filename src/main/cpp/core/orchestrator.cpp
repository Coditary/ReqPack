#include "core/orchestrator.h"

#include "core/downloader.h"
#include "core/planner_core.h"
#include "output/logger.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <utility>

namespace {

bool is_url(const std::string& value) {
    return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0;
}

std::string package_specifier_from_info(const PackageInfo& item) {
    if (item.version.empty()) {
        return item.name;
    }
    return item.name + '@' + item.version;
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
    const std::size_t dot = filename.rfind('.');
    if (dot == std::string::npos || dot == 0) {
        return {};
    }
    std::string ext = filename.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
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
			std::filesystem::remove(tempFile, ec);
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

		if (request.system.empty()) {
			const std::string ext = file_extension(request.localPath);
			if (ext.empty()) {
				Logger::instance().err("cannot determine system for local target with no file extension: " + request.localPath +
					"\nUse: ReqPack install <system> <path>");
				cleanupTempFiles(tempFiles);
				return 1;
			}
			const std::string resolved = this->registry->resolveSystemForExtension(ext);
			if (resolved.empty()) {
				Logger::instance().err("no plugin found for file extension '" + ext + "': " + request.localPath +
					"\nUse: ReqPack install <system> <path>");
				cleanupTempFiles(tempFiles);
				return 1;
			}
			request.system = resolved;
		}

		if (!is_url(request.localPath)) {
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
	}
	// ─────────────────────────────────────────────────────────────────────────

	if (this->requests.front().action == ActionType::LIST) {
		for (const Request& request : this->requests) {
			for (const PackageInfo& item : this->executor->list(request)) {
				Logger::instance().stdout(item.name + " " + item.version + " - " + item.description, request.system, "list");
			}
		}
		return 0;
	}

	if (this->requests.front().action == ActionType::OUTDATED) {
		for (const Request& request : this->requests) {
			for (const PackageInfo& item : this->executor->outdated(request)) {
				Logger::instance().stdout(item.name + " " + item.version + " - " + item.description, request.system, "outdated");
			}
		}
		return 0;
	}

	if (this->requests.front().action == ActionType::SNAPSHOT) {
		(void)this->snapshotExporter->exportSnapshot(this->requests.front());
		return 0;
	}

	if (this->requests.front().action == ActionType::SEARCH) {
		for (const Request& request : this->requests) {
			for (const PackageInfo& item : this->executor->search(request)) {
				Logger::instance().stdout(item.name + " " + item.version + " - " + item.description, request.system, "search");
			}
		}
		return 0;
	}

	if (this->requests.front().action == ActionType::INFO) {
		for (const Request& request : this->requests) {
			const PackageInfo item = this->executor->info(request);
			Logger::instance().stdout(item.name + " " + item.version + " - " + item.description, request.system, "info");
		}
		return 0;
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
