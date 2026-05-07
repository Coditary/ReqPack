#include "core/execution/executor.h"

#include "core/host/host_info.h"
#include "core/planning/request_resolution.h"
#include "output/diagnostic.h"
#include "output/idisplay.h"
#include "output/logger.h"

#include <boost/graph/topological_sort.hpp>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <iterator>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <thread>

namespace {

constexpr const char* INTERNAL_SILENT_RUNTIME_FLAG = "__reqpack-internal-silent-runtime";

DiagnosticMessage resolution_failure_diagnostic(const std::string& details, const std::string& scope) {
	return make_error_diagnostic(
		"executor",
		"Request resolution failed",
		"ReqPack could not map requested input to an executable plugin request.",
		"Check system name, package specifier, and flags, then retry.",
		details,
		"executor",
		scope
	);
}

DiagnosticMessage system_update_failure_diagnostic(const std::string& system) {
	return make_error_diagnostic(
		"executor",
		"System-wide package update failed",
		"Plugin could not complete update-all operation for requested system.",
		"Inspect plugin output above, verify package manager health, then retry update.",
		{},
		system,
		"update"
	);
}

DiagnosticMessage transaction_update_failure_diagnostic(const std::string& system) {
	return make_error_diagnostic(
		"executor",
		"Transaction database update failed",
		"ReqPack could not persist running state for this package operation.",
		"Check transaction database permissions and configured transaction path.",
		{},
		system,
		"transaction"
	);
}

DiagnosticMessage plugin_group_failure_diagnostic(const std::string& system, const std::string& summary, const std::string& cause) {
	return make_error_diagnostic(
		"plugin",
		summary,
		cause,
		"Check plugin installation, registry metadata, and package manager health, then retry command.",
		{},
		system,
		"plugin"
	);
}

DiagnosticMessage package_failure_diagnostic(const std::string& system, const std::string& packageName, bool unavailable) {
	if (unavailable) {
		return make_error_diagnostic(
			"plugin",
			"Package is unavailable: " + system + ":" + packageName,
			"Plugin reported that requested package could not be found or provided by current repositories.",
			"Verify package name, repositories, and system support, then retry.",
			{},
			system + ":" + packageName,
			"package"
		);
	}
	return make_error_diagnostic(
		"plugin",
		"Plugin action failed for " + system + ":" + packageName,
		"Plugin could not complete requested package operation.",
		"Inspect plugin-specific output above and confirm underlying package manager works outside ReqPack.",
		{},
		system + ":" + packageName,
		"package"
	);
}

DiagnosticMessage security_gateway_finding_diagnostic(const ValidationFinding& finding) {
	const std::string summary = finding.message.empty() ? (finding.id.empty() ? finding.kind : finding.id) : finding.message;

	if (finding.kind == "sync_warning") {
		return make_warning_diagnostic(
			"security",
			summary,
			"Security gateway could not use preferred refresh path for requested ecosystems.",
			"Check upstream vulnerability feed availability. ReqPack may fall back to a slower full refresh.",
			{},
			finding.source,
			"gateway"
		);
	}

	if (finding.kind == "sync_error") {
		return make_error_diagnostic(
			"security",
			summary,
			"Security gateway could not prepare vulnerability data required for this command.",
			"Check gateway backend configuration, OSV feed access, and local security index permissions.",
			{},
			finding.source,
			"gateway"
		);
	}

	return make_error_diagnostic(
		"security",
		summary,
		"Security gateway returned an unexpected validation finding.",
		"Inspect security gateway configuration and retry command.",
		{},
		finding.source,
		"gateway"
	);
}

bool has_flag(const std::vector<std::string>& flags, const std::string& name) {
	return std::find(flags.begin(), flags.end(), name) != flags.end();
}

std::string normalize_search_filter_value(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return value;
}

struct PackageResultFilters {
	std::set<std::string> architectures;
	std::set<std::string> packageTypes;
};

PackageResultFilters parse_package_result_filters(const std::vector<std::string>& flags) {
	PackageResultFilters filters;
	for (const auto& flag : flags) {
		if (flag.rfind("arch=", 0) == 0 && flag.size() > 5) {
			filters.architectures.insert(normalize_search_filter_value(flag.substr(5)));
		}
		if (flag.rfind("type=", 0) == 0 && flag.size() > 5) {
			filters.packageTypes.insert(normalize_search_filter_value(flag.substr(5)));
		}
	}
	return filters;
}

bool matches_package_result_filters(const PackageInfo& info, const PackageResultFilters& filters) {
	if (!filters.architectures.empty()) {
		if (info.architecture.empty() || !filters.architectures.contains(normalize_search_filter_value(info.architecture))) {
			return false;
		}
	}
	if (!filters.packageTypes.empty()) {
		if (info.packageType.empty() || !filters.packageTypes.contains(normalize_search_filter_value(info.packageType))) {
			return false;
		}
	}
	return true;
}

std::vector<PackageInfo> apply_package_result_filters(std::vector<PackageInfo> results, const std::vector<std::string>& flags) {
	const PackageResultFilters filters = parse_package_result_filters(flags);
	if (filters.architectures.empty() && filters.packageTypes.empty()) {
		return results;
	}
	results.erase(std::remove_if(results.begin(), results.end(), [&](const PackageInfo& info) {
		return !matches_package_result_filters(info, filters);
	}), results.end());
	return results;
}

bool samePackage(const Package& left, const Package& right) {
	return left.action == right.action &&
		left.system == right.system &&
		left.name == right.name &&
		left.version == right.version &&
		left.sourcePath == right.sourcePath &&
		left.localTarget == right.localTarget;
}

std::string packageRequestSpec(const Package& package) {
	if (package.version.empty()) {
		return package.name;
	}
	return package.name + "-" + package.version;
}

bool actionUsesDesiredStateFilter(const ActionType action) {
	return action == ActionType::INSTALL || action == ActionType::ENSURE || action == ActionType::REMOVE || action == ActionType::UPDATE;
}

bool actionUsesMissingPackageFilter(const ActionType action) {
	return action == ActionType::INSTALL || action == ActionType::ENSURE;
}

bool actionSupportsRecoveryReconciliation(const ActionType action) {
	return action == ActionType::INSTALL || action == ActionType::ENSURE || action == ActionType::REMOVE || action == ActionType::UPDATE;
}

DisplayMode displayModeFromAction(ActionType action) {
		switch (action) {
			case ActionType::INSTALL: return DisplayMode::INSTALL;
			case ActionType::ENSURE:  return DisplayMode::ENSURE;
			case ActionType::REMOVE:  return DisplayMode::REMOVE;
			case ActionType::UPDATE:  return DisplayMode::UPDATE;
			case ActionType::SEARCH:  return DisplayMode::SEARCH;
			case ActionType::LIST:    return DisplayMode::LIST;
			case ActionType::INFO:    return DisplayMode::INFO;
			case ActionType::OUTDATED: return DisplayMode::OUTDATED;
			case ActionType::SNAPSHOT: return DisplayMode::SNAPSHOT;
			case ActionType::SERVE: return DisplayMode::SERVE;
			case ActionType::REMOTE: return DisplayMode::REMOTE;
			case ActionType::SBOM:    return DisplayMode::SBOM;
			default:                  return DisplayMode::IDLE;
		}
}

// Returns true if 'target' is reachable from any vertex in 'sources' via directed edges.
bool isReachableFromAny(const Graph& graph, const std::set<Graph::vertex_descriptor>& sources, Graph::vertex_descriptor target) {
	if (sources.empty()) {
		return false;
	}
	std::set<Graph::vertex_descriptor> visited;
	std::queue<Graph::vertex_descriptor> bfsQueue;
	for (const Graph::vertex_descriptor src : sources) {
		if (visited.find(src) == visited.end()) {
			visited.insert(src);
			bfsQueue.push(src);
		}
	}
	while (!bfsQueue.empty()) {
		const Graph::vertex_descriptor v = bfsQueue.front();
		bfsQueue.pop();
		if (v == target) {
			return true;
		}
		auto [edgeBegin, edgeEnd] = boost::out_edges(v, graph);
		for (auto edgeIt = edgeBegin; edgeIt != edgeEnd; ++edgeIt) {
			const Graph::vertex_descriptor next = boost::target(*edgeIt, graph);
			if (visited.find(next) == visited.end()) {
				visited.insert(next);
				bfsQueue.push(next);
			}
		}
	}
	return false;
}

// Returns true if at least one package in 'packages' is transitively reachable from any failed vertex.
// When true the caller knows the group depends on a failed predecessor.
bool groupDependsOnFailure(const Graph& graph, const std::vector<Package>& packages, const std::set<Graph::vertex_descriptor>& failedVertices) {
	if (failedVertices.empty()) {
		return false;
	}
	auto [vBegin, vEnd] = boost::vertices(graph);
	for (const Package& pkg : packages) {
		for (auto vIt = vBegin; vIt != vEnd; ++vIt) {
			const Package& vPkg = graph[*vIt];
			if (vPkg.system == pkg.system && vPkg.name == pkg.name && vPkg.action == pkg.action) {
				if (isReachableFromAny(graph, failedVertices, *vIt)) {
					return true;
				}
				break;
			}
		}
	}
	return false;
}

std::string package_identity_key(const Package& package) {
	return package.system + "\n" + package.name + "\n" + package.version;
}

std::string entry_identity_key(const InstalledEntry& entry) {
	return entry.system + "\n" + entry.name + "\n" + entry.version;
}

std::vector<std::string> owner_ids_for_package(const Package& package) {
	std::vector<std::string> ownerIds;
	if (package.directRequest) {
		ownerIds.push_back(installed_root_owner_id(package));
	}
	return ownerIds;
}

bool is_install_like_action(ActionType action) {
	return action == ActionType::INSTALL || action == ActionType::ENSURE || action == ActionType::UPDATE;
}

bool is_remove_action(ActionType action) {
	return action == ActionType::REMOVE;
}

struct ParallelExecutionState {
	std::mutex mutex;
	std::condition_variable condition;
	std::queue<std::size_t> readyIndices;
	std::set<std::string> busySystems;
	std::vector<std::size_t> pendingDependencies;
	std::vector<bool> started;
	std::vector<bool> completed;
	std::vector<bool> failed;
	bool stopLaunching{false};
	std::size_t runningWorkers{0};
};

}  // namespace

Executer::Executer(Registry* registry, const ReqPackConfig& config) : config(config), registry(registry), securityGateway(registry, registry, config) {
	this->registry = registry;
	this->transactionDatabase = std::make_unique<TransactionDatabase>(config);
	// Create HistoryManager when at least one tracking feature is active.
	if (config.history.enabled || config.history.trackInstalled) {
		this->historyManager = std::make_unique<HistoryManager>(config);
	}
}

Executer::~Executer() {}

void Executer::setRequestedItemCount(int count, bool inputAlreadyFiltered) const {
	this->requestedItemCount = std::max(0, count);
	this->inputAlreadyFiltered = inputAlreadyFiltered;
}

std::vector<PackageInfo> Executer::list(const Request& request) const {
	std::string resolutionError;
	const std::optional<Request> resolvedRequest = this->resolveRequest(request, &resolutionError);
	if (!resolvedRequest.has_value()) {
		if (!resolutionError.empty()) {
			Logger::instance().err(resolutionError);
		}
		return {};
	}
	if (this->registry->getPlugin(resolvedRequest->system) == nullptr || !this->registry->loadPlugin(resolvedRequest->system)) {
		return {};
	}
	IPlugin* plugin = this->registry->getPlugin(resolvedRequest->system);
	TaskGroup taskGroup{.action = ActionType::LIST, .system = resolvedRequest->system};
	taskGroup.flags = resolvedRequest->flags;
	return apply_package_result_filters(plugin->list(this->buildPluginContext(plugin, taskGroup)), resolvedRequest->flags);
}

std::vector<PackageInfo> Executer::outdated(const Request& request) const {
	std::string resolutionError;
	const std::optional<Request> resolvedRequest = this->resolveRequest(request, &resolutionError);
	if (!resolvedRequest.has_value()) {
		if (!resolutionError.empty()) {
			Logger::instance().err(resolutionError);
		}
		return {};
	}
	if (this->registry->getPlugin(resolvedRequest->system) == nullptr || !this->registry->loadPlugin(resolvedRequest->system)) {
		return {};
	}
	IPlugin* plugin = this->registry->getPlugin(resolvedRequest->system);
	TaskGroup taskGroup{.action = ActionType::OUTDATED, .system = resolvedRequest->system};
	taskGroup.flags = resolvedRequest->flags;
	return apply_package_result_filters(plugin->outdated(this->buildPluginContext(plugin, taskGroup)), resolvedRequest->flags);
}

std::vector<PackageInfo> Executer::search(const Request& request) const {
	std::string resolutionError;
	const std::optional<Request> resolvedRequest = this->resolveRequest(request, &resolutionError);
	if (!resolvedRequest.has_value()) {
		if (!resolutionError.empty()) {
			Logger::instance().err(resolutionError);
		}
		return {};
	}
	if (this->registry->getPlugin(resolvedRequest->system) == nullptr || !this->registry->loadPlugin(resolvedRequest->system)) {
		return {};
	}
	IPlugin* plugin = this->registry->getPlugin(resolvedRequest->system);
	TaskGroup taskGroup{.action = ActionType::SEARCH, .system = resolvedRequest->system};
	taskGroup.flags = resolvedRequest->flags;
	std::string prompt;
	for (std::size_t index = 0; index < resolvedRequest->packages.size(); ++index) {
		if (index > 0) {
			prompt += ' ';
		}
		prompt += resolvedRequest->packages[index];
	}
	std::vector<PackageInfo> results = plugin->search(this->buildPluginContext(plugin, taskGroup), prompt);
	return apply_package_result_filters(std::move(results), resolvedRequest->flags);
}

PackageInfo Executer::info(const Request& request) const {
	std::string resolutionError;
	const std::optional<Request> resolvedRequest = this->resolveRequest(request, &resolutionError);
	if (!resolvedRequest.has_value()) {
		if (!resolutionError.empty()) {
			Logger::instance().err(resolutionError);
		}
		return {};
	}
	if (this->registry->getPlugin(resolvedRequest->system) == nullptr || !this->registry->loadPlugin(resolvedRequest->system)) {
		return {};
	}
	IPlugin* plugin = this->registry->getPlugin(resolvedRequest->system);
	TaskGroup taskGroup{.action = ActionType::INFO, .system = resolvedRequest->system};
	taskGroup.flags = resolvedRequest->flags;
	const std::string packageName = resolvedRequest->packages.empty() ? std::string{} : resolvedRequest->packages.front();
	return plugin->info(this->buildPluginContext(plugin, taskGroup), packageName);
}

std::optional<Package> Executer::resolvePackage(const Request& request, const Package& package) const {
	std::string resolutionError;
	const std::optional<Request> resolvedRequest = this->resolveRequest(request, &resolutionError);
	if (!resolvedRequest.has_value()) {
		if (!resolutionError.empty()) {
			Logger::instance().err(resolutionError);
		}
		return std::nullopt;
	}
	if (this->registry->getPlugin(resolvedRequest->system) == nullptr || !this->registry->loadPlugin(resolvedRequest->system)) {
		return std::nullopt;
	}
	IPlugin* plugin = this->registry->getPlugin(resolvedRequest->system);
	TaskGroup taskGroup{.action = ActionType::SBOM, .system = resolvedRequest->system};
	taskGroup.flags = resolvedRequest->flags;
	Package resolvedPackage = package;
	resolvedPackage.system = resolvedRequest->system;
	if (plugin->supportsResolvePackage()) {
		taskGroup.flags.push_back(INTERNAL_SILENT_RUNTIME_FLAG);
		if (const std::optional<Package> resolved = plugin->resolvePackage(this->buildPluginContext(plugin, taskGroup), resolvedPackage); resolved.has_value()) {
			return resolved;
		}
		return std::nullopt;
	}

	if (!resolvedPackage.version.empty()) {
		return resolvedPackage;
	}

	Request infoRequest = resolvedRequest.value();
	infoRequest.action = ActionType::INFO;
	infoRequest.packages = {resolvedPackage.name};
	infoRequest.flags.push_back(INTERNAL_SILENT_RUNTIME_FLAG);
	const PackageInfo info = this->info(infoRequest);
	if (!info.name.empty() && !info.version.empty() && info.version != "unknown" && info.version != "repo" && info.version != "installed") {
		Package resolved = resolvedPackage;
		resolved.name = info.name;
		resolved.version = info.version;
		return resolved;
	}
	if (info.name.empty() && info.version.empty() && info.description.empty()) {
		return std::nullopt;
	}

	return resolvedPackage;
}

std::optional<Request> Executer::resolveRequest(const Request& request, std::string* errorMessage) const {
	RequestResolutionService resolver(this->registry, this->config);
	return resolver.resolveRequest(request, errorMessage);
}

void Executer::execute(Graph *graph) {
	if (graph == nullptr) {
		return;
	}

	const int requestedItemCount = this->requestedItemCount;
	const bool inputAlreadyFiltered = this->inputAlreadyFiltered;
	this->requestedItemCount = 0;
	this->inputAlreadyFiltered = false;

	if (this->config.execution.useTransactionDb) {
		this->startTransactionDb();
		if (this->transactionDatabase == nullptr || !this->transactionDatabase->ensureReady()) {
			return;
		}
	}

	if (this->config.execution.checkVirtualFileSystemWrite && !this->canWriteToVirtualFileSystem()) {
		return;
	}

	std::vector<TaskGroup> taskGroups;
	if (this->config.execution.useTransactionDb) {
		const std::vector<TaskGroup> recoveryTaskGroups = this->recoverPendingTaskGroups();
		const std::string recoveryRunId = this->activeRunId;
		if (!recoveryTaskGroups.empty() && !recoveryRunId.empty()) {
			const std::vector<TransactionRecord> recoveryRecords = this->executeRecordedTaskGroups(recoveryTaskGroups, recoveryRunId);
			this->writeTransactionResults(recoveryRecords);
			this->markCommittedTransactions();
			if (this->config.execution.deleteCommittedTransactions) {
				this->deleteCommittedTransactions();
			}
		}
		if (this->transactionDatabase->getActiveRun().has_value()) {
			return;
		}
		this->activeRunId.clear();
	}
	const std::vector<TaskGroup> allTaskGroups = this->createTaskGroups(*graph);
	taskGroups = inputAlreadyFiltered ? allTaskGroups : this->filterExecutableTaskGroups(allTaskGroups);
	this->preloadTaskGroups(taskGroups);
	if (this->config.execution.useTransactionDb && !taskGroups.empty()) {
		this->activeRunId.clear();
		std::vector<std::string> runFlags;
		if (!taskGroups.empty()) {
			runFlags = taskGroups.front().flags;
		}
		this->activeRunId = this->transactionDatabase->createRun(this->collectPackages(taskGroups), runFlags);
		if (this->activeRunId.empty()) {
			return;
		}
	}

	// ── Display session begin ─────────────────────────────────────────────────
	bool sessionBegun = false;
	if (!taskGroups.empty()) {
		std::vector<std::string> itemIds;
		for (const TaskGroup& tg : taskGroups) {
			if (tg.usesLocalTarget) {
				itemIds.push_back(tg.system + ":local");
			} else {
				for (const Package& pkg : tg.packages) {
					itemIds.push_back(tg.system + ":" + pkg.name);
				}
			}
		}
		Logger::instance().displaySessionBegin(displayModeFromAction(taskGroups.front().action), itemIds);
		sessionBegun = true;
	}

	const std::vector<TransactionRecord> records = this->executeTaskGroups(taskGroups, graph);
	this->recordHistory(records);
	this->reconcileInstalledOwnership(allTaskGroups, taskGroups, records);
	this->subtractDependencyOwnership(records);
	const std::vector<TransactionRecord> orphanRemovalRecords = this->removeOrphanedDependencies(
		this->historyManager != nullptr ? this->historyManager->loadInstalledState() : std::vector<InstalledEntry>{},
		records
	);
	std::vector<TransactionRecord> allRecords = records;
	allRecords.insert(allRecords.end(), orphanRemovalRecords.begin(), orphanRemovalRecords.end());

	// ── Display session end ───────────────────────────────────────────────────
	if (sessionBegun) {
		int succeeded = 0, failed = 0;
		for (const TransactionRecord& r : allRecords) {
			if (r.status == "success") ++succeeded;
			else ++failed;
		}
		int planned = requestedItemCount;
		if (planned == 0) {
			for (const TaskGroup& tg : allTaskGroups) {
				planned += static_cast<int>(tg.packages.size());
			}
		}
		const int skipped = std::max(0, planned - succeeded - failed);
		Logger::instance().displaySessionEnd(failed == 0, succeeded, skipped, failed);
		Logger::instance().flush();
	}

	if (this->config.execution.useTransactionDb) {
		this->writeTransactionResults(allRecords);
		this->markCommittedTransactions();
		if (this->config.execution.deleteCommittedTransactions) {
			this->deleteCommittedTransactions();
		}
	}
}

std::vector<Package> Executer::normalizedRequirements(const Package& package) const {
	if (package.system.empty() || this->securityGateway.isGatewaySystem(package.system)) {
		return {};
	}

	if (this->registry->getPlugin(package.system) == nullptr || !this->registry->loadPlugin(package.system)) {
		return {};
	}
	IPlugin* plugin = this->registry->getPlugin(package.system);
	if (plugin == nullptr) {
		return {};
	}

	std::vector<Package> requirements;
	for (Package dependency : plugin->getRequirements()) {
		if (dependency.action == ActionType::UNKNOWN) {
			dependency.action = ActionType::INSTALL;
		}
		if (dependency.system.empty()) {
			dependency.system = package.system;
		}
		dependency.system = this->registry->resolvePluginName(dependency.system);
		requirements.push_back(std::move(dependency));
	}
	return requirements;
}

void Executer::reconcileInstalledOwnership(
	const std::vector<TaskGroup>& allTaskGroups,
	const std::vector<TaskGroup>& plannedTaskGroups,
	const std::vector<TransactionRecord>& records
) const {
	if (this->historyManager == nullptr || !this->config.history.trackInstalled) {
		return;
	}

	std::vector<Package> allPackages = this->collectPackages(allTaskGroups);
	std::vector<Package> plannedPackages = this->collectPackages(plannedTaskGroups);
	auto sameRecordPackage = [](const Package& package, const TransactionRecord& record) {
		return package.action == record.action && package.system == record.system && package.name == record.packageName && package.version == record.packageVersion;
	};
	auto findPackageByRecord = [&](const std::vector<Package>& packages, const TransactionRecord& record) -> const Package* {
		for (const Package& package : packages) {
			if (sameRecordPackage(package, record)) {
				return &package;
			}
		}
		return nullptr;
	};
	auto dependencyMatchesPackage = [](const Package& dependency, const Package& candidate) {
		if (dependency.system != candidate.system || dependency.name != candidate.name) {
			return false;
		}
		return dependency.version.empty() || candidate.version.empty() || dependency.version == candidate.version;
	};
	auto mergeOwnershipForPackage = [&](const Package& package) {
		std::vector<std::string> ownerIds = owner_ids_for_package(package);
		for (const Package& candidate : allPackages) {
			if (candidate.system == package.system && candidate.name == package.name && candidate.version == package.version && candidate.action == package.action) {
				continue;
			}
			for (const Package& dependency : this->normalizedRequirements(candidate)) {
				if (dependencyMatchesPackage(dependency, package)) {
					ownerIds.push_back(installed_package_owner_id(candidate));
				}
			}
		}
		ownerIds.erase(std::remove(ownerIds.begin(), ownerIds.end(), std::string{}), ownerIds.end());
		std::sort(ownerIds.begin(), ownerIds.end());
		ownerIds.erase(std::unique(ownerIds.begin(), ownerIds.end()), ownerIds.end());
		if (!ownerIds.empty()) {
			(void)this->historyManager->mergeInstalledOwnership(package, ownerIds, package.directRequest);
		}

		for (Package dependency : this->normalizedRequirements(package)) {
			const std::vector<std::string> dependencyOwners{installed_package_owner_id(package)};
			(void)this->historyManager->mergeInstalledOwnership(dependency, dependencyOwners, false);
		}
	};
	auto wasPlanned = [&](const Package& package) {
		return std::any_of(plannedPackages.begin(), plannedPackages.end(), [&](const Package& candidate) {
			return samePackage(candidate, package);
		});
	};

	for (const TransactionRecord& record : records) {
		if (record.status != "success" || !is_install_like_action(record.action)) {
			continue;
		}

		const Package* plannedPackage = findPackageByRecord(plannedPackages, record);
		if (plannedPackage == nullptr) {
			continue;
		}
		mergeOwnershipForPackage(*plannedPackage);
	}

	for (const Package& package : allPackages) {
		if (!package.directRequest || !is_install_like_action(package.action) || wasPlanned(package)) {
			continue;
		}
		mergeOwnershipForPackage(package);
	}
}

void Executer::subtractDependencyOwnership(const std::vector<TransactionRecord>& records) const {
	if (this->historyManager == nullptr || !this->config.history.trackInstalled) {
		return;
	}

	for (const TransactionRecord& record : records) {
		if (record.status != "success" || !is_remove_action(record.action)) {
			continue;
		}

		Package removedPackage{
			.action = record.action,
			.system = record.system,
			.name = record.packageName,
			.version = record.packageVersion,
		};
		for (Package dependency : this->normalizedRequirements(removedPackage)) {
			const std::vector<std::string> dependencyOwners{installed_package_owner_id(removedPackage)};
			(void)this->historyManager->subtractInstalledOwnership(dependency, dependencyOwners);
		}
	}
}

std::vector<Executer::TransactionRecord> Executer::removeOrphanedDependencies(
	const std::vector<InstalledEntry>& installedState,
	const std::vector<TransactionRecord>& records
) const {
	if (this->historyManager == nullptr || !this->config.history.trackInstalled) {
		return {};
	}

	auto findInstalledDependency = [&](const Package& dependency) -> const InstalledEntry* {
		const InstalledEntry* singleMatch = nullptr;
		for (const InstalledEntry& entry : installedState) {
			if (entry.system != dependency.system || entry.name != dependency.name) {
				continue;
			}
			if (!dependency.version.empty()) {
				if (entry.version == dependency.version) {
					return &entry;
				}
				continue;
			}
			if (singleMatch != nullptr) {
				return nullptr;
			}
			singleMatch = &entry;
		}
		return singleMatch;
	};

	std::vector<Package> orphanedPackages;
	std::set<std::string> queued;
	for (const TransactionRecord& record : records) {
		if (record.status != "success" || !is_remove_action(record.action)) {
			continue;
		}

		Package removedPackage{
			.action = record.action,
			.system = record.system,
			.name = record.packageName,
			.version = record.packageVersion,
		};
		for (Package dependency : this->normalizedRequirements(removedPackage)) {
			dependency.action = ActionType::REMOVE;
			const InstalledEntry* installedDependency = findInstalledDependency(dependency);
			if (installedDependency == nullptr) {
				continue;
			}
			if (!installedDependency->owners.empty()) {
				continue;
			}
			dependency.version = installedDependency->version;
			const std::string identity = entry_identity_key(*installedDependency);
			if (!queued.insert(identity).second) {
				continue;
			}
			orphanedPackages.push_back(std::move(dependency));
		}
	}

	if (orphanedPackages.empty()) {
		return {};
	}

	std::map<std::string, std::vector<Package>> packagesBySystem;
	for (const Package& package : orphanedPackages) {
		packagesBySystem[package.system].push_back(package);
	}

	std::vector<TransactionRecord> orphanRecords;
	for (const auto& [system, packages] : packagesBySystem) {
		TaskGroup taskGroup{.action = ActionType::REMOVE, .system = system, .packages = packages};
		std::vector<TransactionRecord> batchRecords = this->executeTaskGroup(taskGroup, {});
		orphanRecords.insert(orphanRecords.end(), batchRecords.begin(), batchRecords.end());
	}

	if (!orphanRecords.empty()) {
		this->recordHistory(orphanRecords);
		this->subtractDependencyOwnership(orphanRecords);
		std::vector<InstalledEntry> reloadedState = this->historyManager->loadInstalledState();
		std::vector<TransactionRecord> nested = this->removeOrphanedDependencies(reloadedState, orphanRecords);
		orphanRecords.insert(orphanRecords.end(), nested.begin(), nested.end());
	}

	return orphanRecords;
}

bool Executer::updateSystem(const Request& request) const {
	const std::vector<bool> results = this->updateSystems({request});
	return !results.empty() && results.front();
}

std::vector<bool> Executer::updateSystems(const std::vector<Request>& requests) const {
	std::vector<bool> results(requests.size(), false);
	if (requests.empty()) {
		return results;
	}

	struct UpdateTask {
		std::size_t requestIndex{0};
		TaskGroup taskGroup;
	};

	std::vector<UpdateTask> updateTasks;
	updateTasks.reserve(requests.size());
	for (std::size_t index = 0; index < requests.size(); ++index) {
		const Request& request = requests[index];
		if (request.system.empty() || request.usesLocalTarget || !request.packages.empty()) {
			continue;
		}

		std::string errorMessage;
		const std::optional<Request> resolvedRequest = this->resolveRequest(request, &errorMessage);
		if (!resolvedRequest.has_value()) {
			if (!errorMessage.empty()) {
				Logger::instance().diagnostic(resolution_failure_diagnostic(errorMessage, "update"));
			}
			continue;
		}

		updateTasks.push_back(UpdateTask{
			.requestIndex = index,
			.taskGroup = TaskGroup{
				.action = ActionType::UPDATE,
				.system = resolvedRequest->system,
				.flags = resolvedRequest->flags,
			}
		});
	}

	if (updateTasks.empty()) {
		return results;
	}

	std::vector<TaskGroup> preloadGroups;
	preloadGroups.reserve(updateTasks.size());
	for (const UpdateTask& task : updateTasks) {
		preloadGroups.push_back(task.taskGroup);
	}
	this->preloadTaskGroups(preloadGroups);
	for (std::size_t index = 0; index < updateTasks.size(); ++index) {
		updateTasks[index].taskGroup = preloadGroups[index];
	}

	auto execute_update = [&](const TaskGroup& taskGroup) {
		if (this->securityGateway.isGatewaySystem(taskGroup.system)) {
			return this->dispatchTaskGroupToSecurityGateway(taskGroup);
		}
		if (taskGroup.pluginLoadFailed) {
			return false;
		}
		return this->dispatchTaskGroupToPlugin(taskGroup);
	};

	auto run_task = [&](const UpdateTask& task) {
		Logger::instance().displayItemBegin(task.taskGroup.system, task.taskGroup.system);
		Logger::instance().displayItemStep(task.taskGroup.system, "update all packages");
		const bool ok = execute_update(task.taskGroup);
		if (ok) {
			Logger::instance().displayItemSuccess(task.taskGroup.system);
		} else {
			Logger::instance().displayItemFailure(task.taskGroup.system, system_update_failure_diagnostic(task.taskGroup.system));
		}
		results[task.requestIndex] = ok;
		return ok;
	};

	if (resolved_execution_jobs(this->config) <= 1 || updateTasks.size() <= 1) {
		for (const UpdateTask& task : updateTasks) {
			const bool ok = run_task(task);
			if (!ok && this->config.execution.stopOnFirstFailure) {
				break;
			}
		}
		return results;
	}

	ParallelExecutionState state;
	for (std::size_t index = 0; index < updateTasks.size(); ++index) {
		state.readyIndices.push(index);
	}
	state.started.assign(updateTasks.size(), false);
	state.completed.assign(updateTasks.size(), false);
	state.failed.assign(updateTasks.size(), false);

	auto worker = [&]() {
		for (;;) {
			std::size_t index = 0;
			UpdateTask task;
			{
				std::unique_lock<std::mutex> lock(state.mutex);
				for (;;) {
					if (state.stopLaunching || (state.readyIndices.empty() && state.runningWorkers == 0)) {
						return;
					}

					const std::size_t readyCount = state.readyIndices.size();
					bool found = false;
					for (std::size_t attempt = 0; attempt < readyCount; ++attempt) {
						const std::size_t candidate = state.readyIndices.front();
						state.readyIndices.pop();
						if (state.started[candidate]) {
							continue;
						}
						const std::string& system = updateTasks[candidate].taskGroup.system;
						if (state.busySystems.contains(system)) {
							state.readyIndices.push(candidate);
							continue;
						}
						state.started[candidate] = true;
						state.busySystems.insert(system);
						++state.runningWorkers;
						index = candidate;
						task = updateTasks[candidate];
						found = true;
						break;
					}
					if (found) {
						break;
					}
					if (state.runningWorkers == 0) {
						return;
					}
					state.condition.wait(lock);
				}
			}

			const bool ok = run_task(task);
			{
				std::lock_guard<std::mutex> lock(state.mutex);
				state.completed[index] = true;
				state.failed[index] = !ok;
				state.busySystems.erase(task.taskGroup.system);
				if (!ok && this->config.execution.stopOnFirstFailure) {
					state.stopLaunching = true;
				}
				if (state.runningWorkers > 0) {
					--state.runningWorkers;
				}
			}
			state.condition.notify_all();
		}
	};

	const std::size_t workerCount = std::min<std::size_t>(resolved_execution_jobs(this->config), updateTasks.size());
	std::vector<std::thread> workers;
	workers.reserve(workerCount);
	for (std::size_t index = 0; index < workerCount; ++index) {
		workers.emplace_back(worker);
	}
	for (std::thread& thread : workers) {
		if (thread.joinable()) {
			thread.join();
		}
	}

	return results;
}




















































void Executer::startTransactionDb() const {
	if (this->transactionDatabase != nullptr) {
		(void)this->transactionDatabase->ensureReady();
	}
}

bool Executer::canWriteToVirtualFileSystem() const {
	// Skeleton hook: later this will check VFS write permissions and locks.
	return true;
}

std::vector<Executer::TaskGroup> Executer::recoverPendingTaskGroups() const {
	std::vector<TaskGroup> taskGroups;
	if (this->transactionDatabase == nullptr) {
		return taskGroups;
	}

	const std::optional<TransactionRunRecord> activeRun = this->transactionDatabase->getActiveRun();
	if (!activeRun.has_value() || activeRun->state == "committed") {
		return taskGroups;
	}

	this->activeRunId = activeRun->id;
	const std::vector<TransactionItemRecord> items = this->transactionDatabase->getRunItems(activeRun->id);
	std::vector<TransactionItemRecord> pendingItems;
	for (const TransactionItemRecord& item : items) {
		if (item.status == "success" || item.status == "committed" || item.status == "failed") {
			continue;
		}
		pendingItems.push_back(item);
	}
	pendingItems = this->reconcileRecoveredItems(activeRun->id, pendingItems);

	if (pendingItems.empty()) {
		(void)this->transactionDatabase->markRunCommitted(activeRun->id);
		if (this->config.execution.deleteCommittedTransactions) {
			(void)this->transactionDatabase->deleteRun(activeRun->id);
		}
		this->activeRunId.clear();
		return {};
	}

	(void)this->transactionDatabase->markRunState(activeRun->id, "recovered");
	std::vector<TaskGroup> recoveredTaskGroups = this->createTaskGroupsFromRecords(pendingItems);
	for (TaskGroup& taskGroup : recoveredTaskGroups) {
		taskGroup.flags = activeRun->flags;
	}
	return recoveredTaskGroups;
}

std::vector<TransactionItemRecord> Executer::reconcileRecoveredItems(const std::string& runId, const std::vector<TransactionItemRecord>& items) const {
	std::vector<TransactionItemRecord> pendingItems;
	if (this->transactionDatabase == nullptr) {
		return items;
	}

	std::vector<TransactionItemRecord> recoverableItems;
	for (const TransactionItemRecord& item : items) {
		if (!actionSupportsRecoveryReconciliation(item.package.action)) {
			pendingItems.push_back(item);
			continue;
		}

		const std::string resolvedSystem = this->registry->resolvePluginName(item.package.system);
		if (this->registry->getPlugin(resolvedSystem) == nullptr || !this->registry->loadPlugin(resolvedSystem)) {
			pendingItems.push_back(item);
			continue;
		}

		TransactionItemRecord recoverableItem = item;
		recoverableItem.package.system = resolvedSystem;
		recoverableItems.push_back(std::move(recoverableItem));
	}

	std::vector<std::string> checkedSystems;
	for (const TransactionItemRecord& item : recoverableItems) {
		if (std::find(checkedSystems.begin(), checkedSystems.end(), item.package.system) != checkedSystems.end()) {
			continue;
		}
		checkedSystems.push_back(item.package.system);

		std::vector<TransactionItemRecord> systemItems;
		std::vector<Package> systemPackages;
		for (const TransactionItemRecord& candidate : recoverableItems) {
			if (candidate.package.system != item.package.system) {
				continue;
			}
			systemItems.push_back(candidate);
			systemPackages.push_back(candidate.package);
		}

		IPlugin* plugin = this->registry->getPlugin(item.package.system);
		if (plugin == nullptr) {
			pendingItems.insert(pendingItems.end(), systemItems.begin(), systemItems.end());
			continue;
		}

		// Recovery can resume safely only after re-checking which recorded items still need their desired end state.
		const std::vector<Package> missingPackages = plugin->getMissingPackages(systemPackages);
		for (const TransactionItemRecord& systemItem : systemItems) {
			const bool isMissing = std::any_of(missingPackages.begin(), missingPackages.end(), [&](const Package& missingPackage) {
				return samePackage(missingPackage, systemItem.package);
			});
			if (isMissing) {
				pendingItems.push_back(systemItem);
				continue;
			}

			if (!this->transactionDatabase->updateItemStatus(runId, systemItem.package, "success")) {
				pendingItems.push_back(systemItem);
			}
		}
	}

	return pendingItems;
}

std::vector<Executer::TaskGroup> Executer::createTaskGroupsFromRecords(const std::vector<TransactionItemRecord>& records) const {
	std::vector<TaskGroup> groups;

	for (const TransactionItemRecord& record : records) {
		if (groups.empty() || groups.back().action != record.package.action || groups.back().system != record.package.system) {
			groups.push_back(TaskGroup{.action = record.package.action, .system = record.package.system, .flags = record.package.flags});
		}
		groups.back().packages.push_back(record.package);
		if (record.package.localTarget) {
			groups.back().usesLocalTarget = true;
			groups.back().localPath = record.package.sourcePath;
		}
	}

	return groups;
}

std::vector<Package> Executer::collectPackages(const std::vector<TaskGroup>& taskGroups) const {
	std::vector<Package> packages;
	for (const TaskGroup& taskGroup : taskGroups) {
		packages.insert(packages.end(), taskGroup.packages.begin(), taskGroup.packages.end());
	}
	return packages;
}

std::vector<Graph::vertex_descriptor> Executer::orderedVertices(const Graph& graph) const {
	const auto numVertices = boost::num_vertices(graph);
	if (numVertices == 0) {
		return {};
	}

	std::vector<Graph::vertex_descriptor> order;
	boost::topological_sort(graph, std::back_inserter(order));
	std::reverse(order.begin(), order.end());

	std::vector<int> levels(numVertices, 0);
	for (const Graph::vertex_descriptor v : order) {
		auto [edgeBegin, edgeEnd] = boost::out_edges(v, graph);
		for (auto edgeIt = edgeBegin; edgeIt != edgeEnd; ++edgeIt) {
			const Graph::vertex_descriptor target = boost::target(*edgeIt, graph);
			if (levels[target] <= levels[v]) {
				levels[target] = levels[v] + 1;
			}
		}
	}

	std::stable_sort(order.begin(), order.end(), [&](const Graph::vertex_descriptor a, const Graph::vertex_descriptor b) {
		if (levels[a] != levels[b]) {
			return levels[a] < levels[b];
		}
		return graph[a].system < graph[b].system;
	});

	return order;
}

std::vector<Executer::TaskGroup> Executer::createTaskGroups(const Graph& graph) const {
	std::vector<TaskGroup> groups;

	for (const Graph::vertex_descriptor vertex : this->orderedVertices(graph)) {
		const Package& package = graph[vertex];
		if (groups.empty() || groups.back().action != package.action || groups.back().system != package.system) {
			groups.push_back(TaskGroup{.action = package.action, .system = package.system, .flags = package.flags});
		}

		groups.back().packages.push_back(package);
		if (package.localTarget) {
			groups.back().usesLocalTarget = true;
			groups.back().localPath = package.sourcePath;
		}
	}

	return groups;
}

void Executer::preloadTaskGroups(std::vector<TaskGroup>& taskGroups) const {
	for (TaskGroup& taskGroup : taskGroups) {
		taskGroup.plugin = nullptr;
		taskGroup.pluginLoadFailed = false;
		if (this->securityGateway.isGatewaySystem(taskGroup.system)) {
			continue;
		}
		if (this->registry->getPlugin(taskGroup.system) == nullptr || !this->registry->loadPlugin(taskGroup.system)) {
			taskGroup.pluginLoadFailed = true;
			continue;
		}
		taskGroup.plugin = this->registry->getPlugin(taskGroup.system);
		if (taskGroup.plugin == nullptr) {
			taskGroup.pluginLoadFailed = true;
		}
	}
}

std::vector<Executer::TaskGroupPlan> Executer::createTaskGroupPlans(const std::vector<TaskGroup>& taskGroups, const Graph* graph) const {
	std::vector<TaskGroupPlan> plans;
	plans.reserve(taskGroups.size());
	for (const TaskGroup& taskGroup : taskGroups) {
		plans.push_back(TaskGroupPlan{.taskGroup = taskGroup});
	}
	if (graph == nullptr || taskGroups.empty()) {
		return plans;
	}

	std::map<std::string, std::size_t> packageToGroupIndex;
	for (std::size_t groupIndex = 0; groupIndex < taskGroups.size(); ++groupIndex) {
		for (const Package& package : taskGroups[groupIndex].packages) {
			packageToGroupIndex[package_identity_key(package)] = groupIndex;
		}
	}

	std::set<std::pair<std::size_t, std::size_t>> seenEdges;
	auto [edgeBegin, edgeEnd] = boost::edges(*graph);
	for (auto edgeIt = edgeBegin; edgeIt != edgeEnd; ++edgeIt) {
		const Package& sourcePackage = (*graph)[boost::source(*edgeIt, *graph)];
		const Package& targetPackage = (*graph)[boost::target(*edgeIt, *graph)];
		const auto sourceGroupIt = packageToGroupIndex.find(package_identity_key(sourcePackage));
		const auto targetGroupIt = packageToGroupIndex.find(package_identity_key(targetPackage));
		if (sourceGroupIt == packageToGroupIndex.end() || targetGroupIt == packageToGroupIndex.end()) {
			continue;
		}
		if (sourceGroupIt->second == targetGroupIt->second) {
			continue;
		}
		const std::pair<std::size_t, std::size_t> edge{sourceGroupIt->second, targetGroupIt->second};
		if (!seenEdges.insert(edge).second) {
			continue;
		}
		plans[sourceGroupIt->second].successors.push_back(targetGroupIt->second);
		++plans[targetGroupIt->second].pendingDependencies;
	}

	return plans;
}

std::vector<Executer::TaskGroup> Executer::filterExecutableTaskGroups(const std::vector<TaskGroup>& taskGroups) const {
	std::vector<TaskGroup> filteredTaskGroups;

	for (const TaskGroup& taskGroup : taskGroups) {
		if (taskGroup.packages.empty()) {
			continue;
		}

		if (!actionUsesMissingPackageFilter(taskGroup.action)) {
			filteredTaskGroups.push_back(taskGroup);
			continue;
		}

		IPlugin* plugin = this->registry->getPlugin(taskGroup.system);
		if (this->securityGateway.isGatewaySystem(taskGroup.system)) {
			filteredTaskGroups.push_back(taskGroup);
			continue;
		}
		if (plugin == nullptr || !this->registry->loadPlugin(taskGroup.system)) {
			filteredTaskGroups.push_back(taskGroup);
			continue;
		}

		TaskGroup filteredTaskGroup = taskGroup;
		if (filteredTaskGroup.usesLocalTarget) {
			filteredTaskGroups.push_back(std::move(filteredTaskGroup));
			continue;
		}
		filteredTaskGroup.packages = plugin->getMissingPackages(taskGroup.packages);
		if (!filteredTaskGroup.packages.empty()) {
			filteredTaskGroups.push_back(std::move(filteredTaskGroup));
		}
	}

	return filteredTaskGroups;
}

std::vector<Executer::TransactionRecord> Executer::executeTaskGroups(const std::vector<TaskGroup>& taskGroups, const Graph* graph) const {
	return this->executeRecordedTaskGroups(taskGroups, this->activeRunId, graph);
}

std::vector<Executer::TransactionRecord> Executer::executeRecordedTaskGroups(const std::vector<TaskGroup>& taskGroups, const std::string& runId, const Graph* graph) const {
	std::vector<TaskGroupPlan> plans = this->createTaskGroupPlans(taskGroups, graph);

	auto has_failure = [](const std::vector<TransactionRecord>& groupRecords) {
		return std::any_of(groupRecords.begin(), groupRecords.end(), [](const TransactionRecord& record) {
			return record.status == "failed";
		});
	};

	auto run_sequential = [&]() {
		std::vector<TransactionRecord> records;
		std::vector<std::size_t> pendingDependencies;
		std::vector<bool> blocked(plans.size(), false);
		pendingDependencies.reserve(plans.size());
		for (const TaskGroupPlan& plan : plans) {
			pendingDependencies.push_back(plan.pendingDependencies);
		}

		for (std::size_t index = 0; index < plans.size(); ++index) {
			if (blocked[index] || pendingDependencies[index] != 0) {
				continue;
			}

			const std::vector<TransactionRecord> groupRecords = this->executeTaskGroup(plans[index].taskGroup, runId);
			records.insert(records.end(), groupRecords.begin(), groupRecords.end());
			const bool failed = has_failure(groupRecords);

			for (const std::size_t successor : plans[index].successors) {
				if (pendingDependencies[successor] > 0) {
					--pendingDependencies[successor];
				}
				if (failed) {
					blocked[successor] = true;
				}
			}

			if (failed && this->config.execution.stopOnFirstFailure) {
				break;
			}
		}

		return records;
	};

	if (graph == nullptr || resolved_execution_jobs(this->config) <= 1 || taskGroups.size() <= 1) {
		return run_sequential();
	}

	std::vector<std::vector<TransactionRecord>> resultSlots(plans.size());
	ParallelExecutionState state;
	state.pendingDependencies.reserve(plans.size());
	state.started.assign(plans.size(), false);
	state.completed.assign(plans.size(), false);
	state.failed.assign(plans.size(), false);
	for (const TaskGroupPlan& plan : plans) {
		state.pendingDependencies.push_back(plan.pendingDependencies);
	}
	for (std::size_t index = 0; index < plans.size(); ++index) {
		if (state.pendingDependencies[index] == 0) {
			state.readyIndices.push(index);
		}
	}

	auto worker = [&]() {
		for (;;) {
			std::size_t index = 0;
			TaskGroup taskGroup;
			{
				std::unique_lock<std::mutex> lock(state.mutex);
				for (;;) {
					if (state.stopLaunching || (state.readyIndices.empty() && state.runningWorkers == 0)) {
						return;
					}

					const std::size_t readyCount = state.readyIndices.size();
					bool found = false;
					for (std::size_t attempt = 0; attempt < readyCount; ++attempt) {
						const std::size_t candidate = state.readyIndices.front();
						state.readyIndices.pop();
						if (state.started[candidate] || state.failed[candidate] || state.pendingDependencies[candidate] != 0) {
							continue;
						}
						const std::string& system = plans[candidate].taskGroup.system;
						if (state.busySystems.contains(system)) {
							state.readyIndices.push(candidate);
							continue;
						}
						state.started[candidate] = true;
						state.busySystems.insert(system);
						++state.runningWorkers;
						index = candidate;
						taskGroup = plans[candidate].taskGroup;
						found = true;
						break;
					}

					if (found) {
						break;
					}
					if (state.runningWorkers == 0) {
						return;
					}
					state.condition.wait(lock);
				}
			}

			std::vector<TransactionRecord> groupRecords = this->executeTaskGroup(taskGroup, runId);
			const bool failed = has_failure(groupRecords);

			{
				std::lock_guard<std::mutex> lock(state.mutex);
				resultSlots[index] = std::move(groupRecords);
				state.completed[index] = true;
				state.failed[index] = failed;
				state.busySystems.erase(taskGroup.system);
				for (const std::size_t successor : plans[index].successors) {
					if (state.pendingDependencies[successor] > 0) {
						--state.pendingDependencies[successor];
					}
					if (failed) {
						state.failed[successor] = true;
					}
					if (state.pendingDependencies[successor] == 0 && !state.started[successor] && !state.completed[successor] && !state.failed[successor]) {
						state.readyIndices.push(successor);
					}
				}
				if (failed && this->config.execution.stopOnFirstFailure) {
					state.stopLaunching = true;
				}
				if (state.runningWorkers > 0) {
					--state.runningWorkers;
				}
			}
			state.condition.notify_all();
		}
	};

	const std::size_t workerCount = std::min<std::size_t>(resolved_execution_jobs(this->config), plans.size());
	std::vector<std::thread> workers;
	workers.reserve(workerCount);
	for (std::size_t index = 0; index < workerCount; ++index) {
		workers.emplace_back(worker);
	}
	for (std::thread& thread : workers) {
		if (thread.joinable()) {
			thread.join();
		}
	}

	std::vector<TransactionRecord> records;
	for (const auto& slot : resultSlots) {
		records.insert(records.end(), slot.begin(), slot.end());
	}
	return records;
}

std::vector<Executer::TransactionRecord> Executer::executeTaskGroup(const TaskGroup& taskGroup) const {
	return this->executeTaskGroup(taskGroup, this->activeRunId);
}

PluginCallContext Executer::buildPluginContext(IPlugin* plugin, const TaskGroup& taskGroup) const {
	if (plugin == nullptr) {
		return {};
	}

	std::string itemId;
	if (taskGroup.usesLocalTarget) {
		itemId = taskGroup.system + ":local";
	} else if (taskGroup.packages.size() == 1) {
		itemId = taskGroup.system + ":" + taskGroup.packages.front().name;
	}

	return PluginCallContext{
		.pluginId = plugin->getPluginId(),
		.pluginDirectory = plugin->getPluginDirectory(),
		.scriptPath = plugin->getScriptPath(),
		.bootstrapPath = plugin->getBootstrapPath(),
		.flags = taskGroup.flags,
		.host = plugin->getRuntimeHost(),
		.proxy = proxy_config_for_system(this->config, plugin->getPluginId()),
		.currentItemId = itemId,
		.repositories = repositories_for_ecosystem(this->config, plugin->getPluginId()),
		.hostInfo = HostInfoService::currentSnapshot()
	};
}

std::vector<Executer::TransactionRecord> Executer::executeTaskGroup(const TaskGroup& taskGroup, const std::string& runId) const {
	if (taskGroup.packages.empty() && !taskGroup.usesLocalTarget) {
		return {};
	}

	if (this->securityGateway.isGatewaySystem(taskGroup.system)) {
		const bool ok = this->dispatchTaskGroupToSecurityGateway(taskGroup);
		std::vector<TransactionRecord> records = ok ? this->buildSuccessRecords(taskGroup) : this->buildFailureRecords(taskGroup);
		for (TransactionRecord& record : records) {
			record.runId = runId;
			if (!ok) {
				record.errorMessage = "security gateway action failed";
			}
		}
		return records;
	}

	if (taskGroup.pluginLoadFailed || (taskGroup.plugin == nullptr && (this->registry->getPlugin(taskGroup.system) == nullptr || !this->registry->loadPlugin(taskGroup.system)))) {
		std::vector<TransactionRecord> records = this->buildFailureRecords(taskGroup);
		for (TransactionRecord& record : records) {
			record.runId = runId;
			record.errorMessage = "plugin load failed";
		}
		Logger::instance().diagnostic(plugin_group_failure_diagnostic(
			taskGroup.system,
			"Plugin load failed for system '" + taskGroup.system + "'",
			"ReqPack could not load requested plugin implementation before executing operation."
		));
		return records;
	}

	if (this->config.execution.useTransactionDb && this->transactionDatabase != nullptr && !runId.empty()) {
		return this->executeTransactionalTaskGroup(taskGroup, runId);
	}

	if (!this->dispatchTaskGroupToPlugin(taskGroup)) {
		std::vector<TransactionRecord> records = this->buildFailureRecords(taskGroup);
		for (TransactionRecord& record : records) {
			record.runId = runId;
			record.errorMessage = "plugin action failed";
		}
		Logger::instance().diagnostic(plugin_group_failure_diagnostic(
			taskGroup.system,
			"Plugin action failed for system '" + taskGroup.system + "'",
			"Plugin loaded but returned failure while processing requested action."
		));
		return records;
	}

	std::vector<TransactionRecord> records = this->buildSuccessRecords(taskGroup);
	for (TransactionRecord& record : records) {
		record.runId = runId;
	}
	return records;
}

std::vector<Executer::TransactionRecord> Executer::executeTransactionalTaskGroup(const TaskGroup& taskGroup, const std::string& runId) const {
	if (taskGroup.usesLocalTarget) {
		if (!this->dispatchTaskGroupToPlugin(taskGroup)) {
			std::vector<TransactionRecord> failureRecords = this->buildFailureRecords(taskGroup);
			for (TransactionRecord& record : failureRecords) {
				record.runId = runId;
				record.errorMessage = "plugin action failed";
			}
			Logger::instance().diagnostic(plugin_group_failure_diagnostic(
				taskGroup.system,
				"Local target install failed for system '" + taskGroup.system + "'",
				"Plugin could not process requested local file or archive target."
			));
			return failureRecords;
		}

		std::vector<TransactionRecord> successRecords = this->buildSuccessRecords(taskGroup);
		for (TransactionRecord& record : successRecords) {
			record.runId = runId;
		}
		return successRecords;
	}

	std::vector<TransactionRecord> records;
	records.reserve(taskGroup.packages.size());

	auto appendSuccessRecords = [&](const std::vector<Package>& packages) {
		if (packages.empty()) {
			return;
		}
		TaskGroup resultTaskGroup = taskGroup;
		resultTaskGroup.packages = packages;
		std::vector<TransactionRecord> successRecords = this->buildSuccessRecords(resultTaskGroup);
		for (TransactionRecord& record : successRecords) {
			record.runId = runId;
		}
		records.insert(records.end(), successRecords.begin(), successRecords.end());
	};

	auto appendFailureRecords = [&](const std::vector<Package>& packages, const std::string& errorMessage) {
		if (packages.empty()) {
			return;
		}
		TaskGroup resultTaskGroup = taskGroup;
		resultTaskGroup.packages = packages;
		std::vector<TransactionRecord> failureRecords = this->buildFailureRecords(resultTaskGroup);
		for (TransactionRecord& record : failureRecords) {
			record.runId = runId;
			record.errorMessage = errorMessage;
		}
		records.insert(records.end(), failureRecords.begin(), failureRecords.end());
	};

	auto appendFailureRecord = [&](const Package& package, const std::string& errorMessage) {
		TaskGroup resultTaskGroup = taskGroup;
		resultTaskGroup.packages = {package};
		std::vector<TransactionRecord> failureRecords = this->buildFailureRecords(resultTaskGroup);
		for (TransactionRecord& record : failureRecords) {
			record.runId = runId;
			record.errorMessage = errorMessage;
		}
		records.insert(records.end(), failureRecords.begin(), failureRecords.end());
	};

	auto displaySuccess = [&](const std::vector<Package>& packages) {
		for (const Package& package : packages) {
			Logger::instance().displayItemSuccess(taskGroup.system + ":" + package.name);
		}
	};

	auto displayFailure = [&](const std::vector<Package>& packages, const std::string& reason) {
		for (const Package& package : packages) {
			Logger::instance().displayItemFailure(taskGroup.system + ":" + package.name, reason);
		}
	};

	auto containsPackage = [](const std::vector<Package>& packages, const Package& candidate) {
		return std::any_of(packages.begin(), packages.end(), [&](const Package& package) {
			return samePackage(package, candidate);
		});
	};

	for (const Package& package : taskGroup.packages) {
		Logger::instance().displayItemBegin(taskGroup.system + ":" + package.name, package.name);
	}

	const TaskGroup& executableTaskGroup = taskGroup;

	if (!this->transactionDatabase->updateItemsStatus(runId, executableTaskGroup.packages, "running")) {
		for (const Package& package : executableTaskGroup.packages) {
			Logger::instance().displayItemFailure(taskGroup.system + ":" + package.name,
			                                   transaction_update_failure_diagnostic(taskGroup.system));
		}
		appendFailureRecords(executableTaskGroup.packages, "transaction update failed");
		return records;
	}

	if (this->dispatchTaskGroupToPlugin(executableTaskGroup)) {
		displaySuccess(executableTaskGroup.packages);
		appendSuccessRecords(executableTaskGroup.packages);
		return records;
	}

	std::set<std::string> unavailablePackages;
	if (IPlugin* plugin = this->registry->getPlugin(taskGroup.system); plugin != nullptr) {
		for (const PluginEventRecord& event : plugin->takeRecentEvents()) {
			if (event.name == "unavailable" && !event.payload.empty()) {
				unavailablePackages.insert(event.payload);
			}
		}
	}

	std::vector<Package> succeededPackages;
	std::vector<Package> failedPackages = executableTaskGroup.packages;
	if (actionUsesDesiredStateFilter(taskGroup.action)) {
		IPlugin* plugin = this->registry->getPlugin(taskGroup.system);
		if (plugin != nullptr) {
			const std::vector<Package> remainingMissingPackages = plugin->getMissingPackages(executableTaskGroup.packages);
			succeededPackages.clear();
			failedPackages.clear();
			for (const Package& package : executableTaskGroup.packages) {
				if (containsPackage(remainingMissingPackages, package)) {
					failedPackages.push_back(package);
				} else {
					succeededPackages.push_back(package);
				}
			}
		}
	}

	displaySuccess(succeededPackages);
	appendSuccessRecords(succeededPackages);
	for (const Package& package : failedPackages) {
		const bool unavailable = unavailablePackages.find(packageRequestSpec(package)) != unavailablePackages.end();
		const std::string errorMessage = unavailable ? "package unavailable" : "plugin action failed";
		Logger::instance().displayItemFailure(taskGroup.system + ":" + package.name,
		                                   package_failure_diagnostic(taskGroup.system, package.name, unavailable));
		appendFailureRecord(package, errorMessage);
	}

	return records;
}

void Executer::writeTransactionResults(const std::vector<TransactionRecord>& records) const {
	if (this->transactionDatabase == nullptr || this->activeRunId.empty()) {
		return;
	}
	const std::vector<TransactionItemRecord> runItems = this->transactionDatabase->getRunItems(this->activeRunId);

	for (const TransactionRecord& record : records) {
		Package package;
		package.action = record.action;
		package.system = record.system;
		package.name = record.packageName;
		package.version = record.packageVersion;
		for (const TransactionItemRecord& item : runItems) {
			if (item.package.action == package.action && item.package.system == package.system && item.package.name == package.name && item.package.version == package.version) {
				package.sourcePath = item.package.sourcePath;
				package.localTarget = item.package.localTarget;
				package.flags = item.package.flags;
				break;
			}
		}
		(void)this->transactionDatabase->updateItemStatus(this->activeRunId, package, record.status, record.errorMessage);
	}
}

void Executer::markCommittedTransactions() const {
	if (this->transactionDatabase == nullptr || this->activeRunId.empty()) {
		return;
	}

	const std::vector<TransactionItemRecord> items = this->transactionDatabase->getRunItems(this->activeRunId);
	// Commit only when every item reached a fully-successful terminal state.
	// Any failed or still-incomplete item means the run must stay active as "failed"
	// so callers can inspect it and recovery logic can distinguish it from a committed run.
	const bool shouldCommit = std::all_of(items.begin(), items.end(), [](const TransactionItemRecord& item) {
		return item.status == "success" || item.status == "committed";
	});
	if (!shouldCommit) {
		(void)this->transactionDatabase->markRunState(this->activeRunId, "failed");
		return;
	}

	(void)this->transactionDatabase->markRunCommitted(this->activeRunId);
}

void Executer::deleteCommittedTransactions() const {
	if (this->transactionDatabase == nullptr || this->activeRunId.empty()) {
		return;
	}

	const std::optional<TransactionRunRecord> activeRun = this->transactionDatabase->getActiveRun();
	if (activeRun.has_value()) {
		return;
	}

	(void)this->transactionDatabase->deleteRun(this->activeRunId);
	this->activeRunId.clear();
}

bool Executer::syncInstalledStateForSystem(const std::string& system, const bool allowEmpty) const {
	if (this->historyManager == nullptr || !this->config.history.trackInstalled || system.empty() || this->securityGateway.isGatewaySystem(system)) {
		return false;
	}
	if (this->registry->getPlugin(system) == nullptr || !this->registry->loadPlugin(system)) {
		return false;
	}

	IPlugin* plugin = this->registry->getPlugin(system);
	if (plugin == nullptr) {
		return false;
	}

	TaskGroup taskGroup{.action = ActionType::LIST, .system = system};
	taskGroup.flags = {INTERNAL_SILENT_RUNTIME_FLAG};
	const std::vector<PackageInfo> installedPackages = plugin->list(this->buildPluginContext(plugin, taskGroup));

	std::vector<InstalledEntry> entries;
	entries.reserve(installedPackages.size());
	for (const PackageInfo& item : installedPackages) {
		if (item.name.empty()) {
			continue;
		}
		entries.push_back(InstalledEntry{
			.name = item.name,
			.version = item.version,
			.system = system,
			.installedAt = {},
		});
	}

	if (entries.empty() && !allowEmpty) {
		return false;
	}

	return this->historyManager->replaceInstalledState(system, entries);
}

std::set<std::string> Executer::refreshInstalledState(const std::vector<TransactionRecord>& records) const {
	std::set<std::string> refreshedSystems;
	if (this->historyManager == nullptr || !this->config.history.trackInstalled) {
		return refreshedSystems;
	}

	std::map<std::string, bool> allowEmptyBySystem;
	for (const TransactionRecord& record : records) {
		if (record.status != "success" || !actionUsesDesiredStateFilter(record.action) || record.system.empty()) {
			continue;
		}

		auto [it, inserted] = allowEmptyBySystem.emplace(record.system, true);
		if (record.action != ActionType::REMOVE) {
			it->second = false;
		}
	}

	for (const auto& [system, allowEmpty] : allowEmptyBySystem) {
		if (this->syncInstalledStateForSystem(system, allowEmpty)) {
			refreshedSystems.insert(system);
		}
	}

	return refreshedSystems;
}

void Executer::recordHistory(const std::vector<TransactionRecord>& records) const {
	if (this->historyManager == nullptr) {
		return;
	}

	auto actionToString = [](ActionType action) -> std::string {
		switch (action) {
			case ActionType::INSTALL: return "install";
			case ActionType::ENSURE:  return "ensure";
			case ActionType::REMOVE:  return "remove";
			case ActionType::UPDATE:  return "update";
			default:                  return "unknown";
		}
	};

	std::vector<HistoryEntry> entries;
	entries.reserve(records.size());
	for (const TransactionRecord& record : records) {
		HistoryEntry entry;
		// timestamp filled in by HistoryManager::record()
		entry.action         = actionToString(record.action);
		entry.packageName    = record.packageName;
		entry.packageVersion = record.packageVersion;
		entry.system         = record.system;
		entry.status         = record.status;
		entry.errorMessage   = record.errorMessage;
		entries.push_back(entry);
		(void)this->historyManager->appendEvent(entry);
	}

	const std::set<std::string> refreshedSystems = this->refreshInstalledState(records);
	for (const HistoryEntry& entry : entries) {
		const bool isMutatingAction = entry.action == "install" || entry.action == "ensure" || entry.action == "remove" || entry.action == "update";
		if (!isMutatingAction || refreshedSystems.find(entry.system) != refreshedSystems.end()) {
			continue;
		}
		(void)this->historyManager->updateInstalledState(entry);
	}
}

std::vector<Package> Executer::orderedPackages(const Graph& graph) const {
	std::vector<Package> packages;
	const std::vector<Graph::vertex_descriptor> order = this->orderedVertices(graph);
	packages.reserve(order.size());
	for (const Graph::vertex_descriptor vertex : order) {
		packages.push_back(graph[vertex]);
	}
	return packages;
}

bool Executer::dispatchTaskGroupToPlugin(const TaskGroup& taskGroup) const {
	if (this->securityGateway.isGatewaySystem(taskGroup.system)) {
		return this->dispatchTaskGroupToSecurityGateway(taskGroup);
	}
	IPlugin* plugin = taskGroup.plugin != nullptr ? taskGroup.plugin : this->registry->getPlugin(taskGroup.system);
	if (plugin == nullptr) {
		return false;
	}
	const PluginCallContext context = this->buildPluginContext(plugin, taskGroup);

		switch (taskGroup.action) {
			case ActionType::INSTALL:
			case ActionType::ENSURE:
				if (taskGroup.usesLocalTarget) {
					return plugin->installLocal(context, taskGroup.localPath);
				}
				return plugin->install(context, taskGroup.packages);
		case ActionType::REMOVE:
			return plugin->remove(context, taskGroup.packages);
		case ActionType::UPDATE:
			return plugin->update(context, taskGroup.packages);
		case ActionType::SEARCH:
		case ActionType::LIST:
		case ActionType::INFO:
		case ActionType::UNKNOWN:
		default:
			return false;
	}
}

bool Executer::dispatchTaskGroupToSecurityGateway(const TaskGroup& taskGroup) const {
	const std::vector<ValidationFinding> findings = this->securityGateway.executeGatewayRequest(taskGroup.action, taskGroup.system, taskGroup.packages);
	for (const ValidationFinding& finding : findings) {
		if (finding.kind == "sync_warning") {
			Logger::instance().diagnostic(security_gateway_finding_diagnostic(finding));
		}
		if (finding.kind == "sync_error") {
			Logger::instance().diagnostic(security_gateway_finding_diagnostic(finding));
			return false;
		}
	}
	return true;
}

std::vector<Executer::TransactionRecord> Executer::buildSuccessRecords(const TaskGroup& taskGroup) const {
	std::vector<TransactionRecord> records;
	records.reserve(taskGroup.packages.size());

	for (const Package& package : taskGroup.packages) {
		records.push_back(TransactionRecord{
			.runId = {},
			.system = taskGroup.system,
			.action = taskGroup.action,
			.packageName = package.name,
			.packageVersion = package.version,
			.status = "success"
		});
	}

	return records;
}

std::vector<Executer::TransactionRecord> Executer::buildFailureRecords(const TaskGroup& taskGroup) const {
	std::vector<TransactionRecord> records;
	records.reserve(taskGroup.packages.size());

	for (const Package& package : taskGroup.packages) {
		records.push_back(TransactionRecord{
			.runId = {},
			.system = taskGroup.system,
			.action = taskGroup.action,
			.packageName = package.name,
			.packageVersion = package.version,
			.status = "failed"
		});
	}

	return records;
}
