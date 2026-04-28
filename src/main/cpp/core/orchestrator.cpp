#include "core/orchestrator.h"

#include "output/logger.h"

#include <filesystem>
#include <utility>

Orchestrator::Orchestrator(std::vector<Request> requests, const ReqPackConfig& config)
	: config(config), requests(std::move(requests)) {
	this->registry  = new Registry(this->config);
	this->planner   = new Planner(this->registry, this->registry->getDatabase(), this->config);
	this->sbomExporter = new SbomExporter(this->registry, this->config);
	this->validator = new Validator(this->registry, this->config);
	this->executor  = new Executer(this->registry, this->config);
}

Orchestrator::~Orchestrator() {
	delete this->registry;
	delete this->planner;
	delete this->sbomExporter;
	delete this->validator;
	delete this->executor;
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
	this->executor->execute(graph);
	delete graph;
}
