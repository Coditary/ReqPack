#include "core/orchestrator.h"

#include "core/downloader.h"
#include "output/logger.h"

#include <algorithm>
#include <filesystem>
#include <utility>

namespace {

bool is_url(const std::string& value) {
    return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0;
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

} // namespace

Orchestrator::Orchestrator(std::vector<Request> requests, const ReqPackConfig& config)
	: config(config), requests(std::move(requests)) {
	this->registry  = new Registry(this->config);
	this->planner   = new Planner(this->registry, this->registry->getDatabase(), this->config);
	this->sbomExporter = new SbomExporter(this->registry, this->config);
	this->snapshotExporter = new SnapshotExporter(this->config);
	this->validator = new Validator(this->registry, this->config);
	this->executor  = new Executer(this->registry, this->config);
}

Orchestrator::~Orchestrator() {
	delete this->registry;
	delete this->planner;
	delete this->sbomExporter;
	delete this->snapshotExporter;
	delete this->validator;
	delete this->executor;
}

int Orchestrator::countRequestedItems() const {
	int count = 0;
	for (const Request& request : this->requests) {
		if (request.action != ActionType::INSTALL && request.action != ActionType::ENSURE &&
			request.action != ActionType::REMOVE && request.action != ActionType::UPDATE) {
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

void Orchestrator::run() {
	(void)this->registry->getDatabase()->ensureReady();
	this->registry->scanDirectory(this->config.registry.pluginDirectory);
	const std::filesystem::path workspacePluginDirectory = std::filesystem::current_path() / "plugins";
	const std::filesystem::path configuredPluginDirectory = std::filesystem::path(this->config.registry.pluginDirectory);
	if (std::filesystem::exists(workspacePluginDirectory) && workspacePluginDirectory != configuredPluginDirectory) {
		this->registry->scanDirectory(workspacePluginDirectory.string());
	}
	if (this->requests.empty()) {
		return;
	}

	// ── URL pre-processing ────────────────────────────────────────────────────
	// For any INSTALL request whose localPath is a URL:
	//   1. If system is empty, resolve it from the file extension via plugin declarations.
	//   2. Download the URL to a temp file and replace localPath with the local path.
	// Temp files are tracked and deleted after execution.
	std::vector<std::filesystem::path> tempFiles;
	for (Request& request : this->requests) {
		if (request.action != ActionType::INSTALL || !request.usesLocalTarget || !is_url(request.localPath)) {
			continue;
		}

		if (request.system.empty()) {
			const std::string ext = file_extension(request.localPath);
			if (ext.empty()) {
				Logger::instance().err("cannot determine system for URL with no file extension: " + request.localPath +
					"\nUse: ReqPack install <system> <url>");
				return;
			}
			const std::string resolved = this->registry->resolveSystemForExtension(ext);
			if (resolved.empty()) {
				Logger::instance().err("no plugin found for file extension '" + ext + "': " + request.localPath +
					"\nUse: ReqPack install <system> <url>");
				return;
			}
			request.system = resolved;
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
			return;
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
		return;
	}

	if (this->requests.front().action == ActionType::OUTDATED) {
		for (const Request& request : this->requests) {
			for (const PackageInfo& item : this->executor->outdated(request)) {
				Logger::instance().stdout(item.name + " " + item.version + " - " + item.description, request.system, "outdated");
			}
		}
		return;
	}

	if (this->requests.front().action == ActionType::SNAPSHOT) {
		(void)this->snapshotExporter->exportSnapshot(this->requests.front());
		return;
	}

	if (this->requests.front().action == ActionType::SEARCH) {
		for (const Request& request : this->requests) {
			for (const PackageInfo& item : this->executor->search(request)) {
				Logger::instance().stdout(item.name + " " + item.version + " - " + item.description, request.system, "search");
			}
		}
		return;
	}

	if (this->requests.front().action == ActionType::INFO) {
		for (const Request& request : this->requests) {
			const PackageInfo item = this->executor->info(request);
			Logger::instance().stdout(item.name + " " + item.version + " - " + item.description, request.system, "info");
		}
		return;
	}

	Graph* graph = this->planner->plan(this->requests);
	if (this->requests.front().action == ActionType::SBOM) {
		if (graph != nullptr) {
			(void)this->sbomExporter->exportGraph(*graph, this->requests.front());
		}
		delete graph;
		return;
	}
	graph = this->validator->validate(graph);
	this->executor->setRequestedItemCount(this->countRequestedItems());
	this->executor->execute(graph);
	delete graph;

	// Clean up any temp files downloaded for URL-based installs.
	for (const std::filesystem::path& tempFile : tempFiles) {
		std::error_code ec;
		std::filesystem::remove(tempFile, ec);
	}
}
