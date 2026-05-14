#include "core/execution/orchestrator.h"

#include "orchestrator_internal.h"

#include "output/diagnostic.h"
#include "output/logger.h"

#include <filesystem>
#include <utility>
#include <vector>

Orchestrator::Orchestrator(std::vector<Request> requests, const ReqPackConfig& config)
	: config(config), requests(std::move(requests)) {
	this->registry = new Registry(this->config);
	this->planner = new Planner(this->registry, this->registry->getDatabase(), this->config);
	this->auditExporter = new AuditExporter(this->registry, this->config);
	this->sbomExporter = new SbomExporter(this->registry, this->config);
	this->snapshotExporter = new SnapshotExporter(this->config);
	this->validator = new Validator(this->registry, this->config);
	this->executor = new Executer(this->registry, this->config);
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
	if (this->shouldRefreshMainRegistry() && !this->registry->getDatabase()->refreshMainRegistry()) {
		Logger::instance().diagnostic(make_warning_diagnostic(
			"registry",
			"Registry refresh failed before update",
			"ReqPack could not synchronize main registry before running update and will continue with cached registry data if available.",
			"Check registry.remoteUrl, network access, and local registry cache if update results look stale.",
			{},
			"registry",
			"update"
		));
	}

	(void)this->registry->getDatabase()->ensureReady();
	this->registry->scanDirectory(this->config.registry.pluginDirectory);
	const std::filesystem::path workspacePluginDirectory = std::filesystem::current_path() / "plugins";
	const std::filesystem::path configuredPluginDirectory = std::filesystem::path(this->config.registry.pluginDirectory);
	if (std::filesystem::exists(workspacePluginDirectory) && workspacePluginDirectory != configuredPluginDirectory) {
		this->registry->scanDirectory(workspacePluginDirectory.string());
	}

	if (this->requests.empty()) {
		return 0;
	}
	if (this->shouldRunSystemWidePackageUpdates()) {
		return this->runSystemWidePackageUpdates();
	}
	if (this->shouldRefreshPluginWrappers()) {
		return this->runPluginWrapperRefresh();
	}
	if (orchestrator_internal::requests_target_plugin_install(this->requests)) {
		return this->runPluginInstallRequests();
	}
	if (orchestrator_internal::requests_target_plugin_remove(this->requests)) {
		return this->runPluginRemoveRequests();
	}

	std::vector<std::filesystem::path> tempFiles;
	if (!orchestrator_internal::prepare_requests_for_run(this->requests, this->registry, this->config, tempFiles)) {
		orchestrator_internal::cleanup_temp_files(tempFiles);
		return 1;
	}

	const ActionType action = this->requests.front().action;
	if (action == ActionType::LIST || action == ActionType::OUTDATED || action == ActionType::SEARCH || action == ActionType::INFO) {
		const int exitCode = orchestrator_internal::run_query_action(action, this->requests, this->executor);
		orchestrator_internal::cleanup_temp_files(tempFiles);
		return exitCode;
	}

	if (action == ActionType::SNAPSHOT) {
		const bool ok = this->snapshotExporter->exportSnapshot(this->requests.front());
		orchestrator_internal::cleanup_temp_files(tempFiles);
		return ok ? 0 : 1;
	}

	if (action == ActionType::PACK) {
		const int exitCode = orchestrator_internal::run_pack_request(this->requests.front(), this->registry, this->config);
		orchestrator_internal::cleanup_temp_files(tempFiles);
		return exitCode;
	}

	std::vector<Request> plannedRequests = this->requests;
	std::vector<std::string> missingSbomPackages;
	if (action == ActionType::AUDIT) {
		plannedRequests = orchestrator_internal::expand_system_only_audit_requests(this->executor, this->requests);
		plannedRequests = orchestrator_internal::resolve_audit_requests(this->executor, plannedRequests);
	} else if (action == ActionType::SBOM) {
		orchestrator_internal::SbomResolutionResult resolvedSbom = orchestrator_internal::resolve_sbom_requests(this->executor, this->config, this->requests);
		plannedRequests = std::move(resolvedSbom.requests);
		missingSbomPackages = std::move(resolvedSbom.missingPackages);
	}
	if (action == ActionType::SBOM && !missingSbomPackages.empty()) {
		orchestrator_internal::log_missing_sbom_packages(missingSbomPackages);
		orchestrator_internal::cleanup_temp_files(tempFiles);
		return 1;
	}

	Graph* graph = this->planner->plan(plannedRequests);
	if (action == ActionType::SBOM) {
		const bool exported = graph != nullptr && this->sbomExporter->exportGraph(*graph, this->requests.front());
		orchestrator_internal::cleanup_temp_files(tempFiles);
		delete graph;
		return exported ? 0 : 1;
	}
	if (action == ActionType::AUDIT) {
		const std::vector<ValidationFinding> findings = this->validator->audit(graph);
		const bool exported = graph != nullptr && this->auditExporter->exportGraph(*graph, findings, this->requests.front());
		delete graph;
		orchestrator_internal::cleanup_temp_files(tempFiles);
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
			orchestrator_internal::log_validation_blocked(this->validator->getLastFindings());
		}
		delete graph;
		orchestrator_internal::cleanup_temp_files(tempFiles);
		return 1;
	}
	graph = validatedGraph;
	this->executor->setRequestedItemCount(this->countRequestedItems(), false);
	const bool executeOk = this->executor->execute(graph);
	delete graph;

	orchestrator_internal::cleanup_temp_files(tempFiles);
	return executeOk ? 0 : 1;
}
