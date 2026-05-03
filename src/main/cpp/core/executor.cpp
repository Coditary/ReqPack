#include "core/executor.h"

#include "output/idisplay.h"
#include "output/logger.h"

#include <boost/graph/topological_sort.hpp>

#include <algorithm>
#include <iterator>
#include <queue>
#include <set>

namespace {

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
		case ActionType::OUTDATED: return DisplayMode::LIST;
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
	if (this->registry->getPlugin(request.system) == nullptr || !this->registry->loadPlugin(request.system)) {
		return {};
	}
	IPlugin* plugin = this->registry->getPlugin(request.system);
	TaskGroup taskGroup{.action = ActionType::LIST, .system = request.system};
	taskGroup.flags = request.flags;
	return plugin->list(this->buildPluginContext(plugin, taskGroup));
}

std::vector<PackageInfo> Executer::outdated(const Request& request) const {
	if (this->registry->getPlugin(request.system) == nullptr || !this->registry->loadPlugin(request.system)) {
		return {};
	}
	IPlugin* plugin = this->registry->getPlugin(request.system);
	TaskGroup taskGroup{.action = ActionType::OUTDATED, .system = request.system};
	taskGroup.flags = request.flags;
	return plugin->outdated(this->buildPluginContext(plugin, taskGroup));
}

std::vector<PackageInfo> Executer::search(const Request& request) const {
	if (this->registry->getPlugin(request.system) == nullptr || !this->registry->loadPlugin(request.system)) {
		return {};
	}
	IPlugin* plugin = this->registry->getPlugin(request.system);
	TaskGroup taskGroup{.action = ActionType::SEARCH, .system = request.system};
	taskGroup.flags = request.flags;
	std::string prompt;
	for (std::size_t index = 0; index < request.packages.size(); ++index) {
		if (index > 0) {
			prompt += ' ';
		}
		prompt += request.packages[index];
	}
	return plugin->search(this->buildPluginContext(plugin, taskGroup), prompt);
}

PackageInfo Executer::info(const Request& request) const {
	if (this->registry->getPlugin(request.system) == nullptr || !this->registry->loadPlugin(request.system)) {
		return {};
	}
	IPlugin* plugin = this->registry->getPlugin(request.system);
	TaskGroup taskGroup{.action = ActionType::INFO, .system = request.system};
	taskGroup.flags = request.flags;
	const std::string packageName = request.packages.empty() ? std::string{} : request.packages.front();
	return plugin->info(this->buildPluginContext(plugin, taskGroup), packageName);
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
	const std::vector<TaskGroup> plannedTaskGroups = inputAlreadyFiltered ? allTaskGroups : this->filterExecutableTaskGroups(allTaskGroups);
	taskGroups = plannedTaskGroups;
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

	// ── History ───────────────────────────────────────────────────────────────
	this->recordHistory(records);

	// ── Display session end ───────────────────────────────────────────────────
	if (sessionBegun) {
		int succeeded = 0, failed = 0;
		for (const TransactionRecord& r : records) {
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
		this->writeTransactionResults(records);
		this->markCommittedTransactions();
		if (this->config.execution.deleteCommittedTransactions) {
			this->deleteCommittedTransactions();
		}
	}
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

std::vector<Executer::TaskGroup> Executer::createTaskGroups(const Graph& graph) const {
	std::vector<TaskGroup> groups;

	for (const Package& package : this->orderedPackages(graph)) {
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

std::vector<Executer::TaskGroup> Executer::filterExecutableTaskGroups(const std::vector<TaskGroup>& taskGroups) const {
	std::vector<TaskGroup> filteredTaskGroups;

	for (const TaskGroup& taskGroup : taskGroups) {
		if (taskGroup.packages.empty()) {
			continue;
		}

		if (!actionUsesDesiredStateFilter(taskGroup.action)) {
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
	std::vector<TransactionRecord> records;
	std::set<Graph::vertex_descriptor> failedVertices;

	for (const TaskGroup& taskGroup : taskGroups) {
		if (this->config.execution.stopOnFirstFailure && !failedVertices.empty()) {
			// When the graph is available use dependency information: only skip
			// groups whose packages are transitively reachable from a failed vertex.
			// Without the graph fall back to the original behaviour (stop everything).
			const bool shouldSkip = (graph == nullptr) || groupDependsOnFailure(*graph, taskGroup.packages, failedVertices);
			if (shouldSkip) {
				// Leave the packages in their initial "planned" state in the DB by
				// not adding any records for this group.
				continue;
			}
		}

		const std::vector<TransactionRecord> groupRecords = this->executeTaskGroup(taskGroup, runId);
		records.insert(records.end(), groupRecords.begin(), groupRecords.end());

		if (this->config.execution.stopOnFirstFailure) {
			const bool hasFailure = std::any_of(groupRecords.begin(), groupRecords.end(), [](const TransactionRecord& record) {
				return record.status == "failed";
			});

			if (hasFailure) {
				if (graph == nullptr) {
					// No graph available (e.g. crash-recovery path): original behaviour.
					break;
				}
				// Record which vertices failed so subsequent groups can be checked.
				auto [vBegin, vEnd] = boost::vertices(*graph);
				for (const TransactionRecord& record : groupRecords) {
					if (record.status != "failed") {
						continue;
					}
					for (auto vIt = vBegin; vIt != vEnd; ++vIt) {
						const Package& vPkg = (*graph)[*vIt];
						if (vPkg.system == record.system && vPkg.name == record.packageName && vPkg.action == record.action) {
							failedVertices.insert(*vIt);
							break;
						}
					}
				}
			}
		}
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
		.currentItemId = itemId
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

	if (this->registry->getPlugin(taskGroup.system) == nullptr || !this->registry->loadPlugin(taskGroup.system)) {
		std::vector<TransactionRecord> records = this->buildFailureRecords(taskGroup);
		for (TransactionRecord& record : records) {
			record.runId = runId;
			record.errorMessage = "plugin load failed";
		}
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
		displayFailure(executableTaskGroup.packages, "transaction update failed");
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
		const std::string errorMessage = unavailablePackages.find(packageRequestSpec(package)) != unavailablePackages.end()
			? "package unavailable"
			: "plugin action failed";
		Logger::instance().displayItemFailure(taskGroup.system + ":" + package.name, errorMessage);
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

	for (const TransactionRecord& record : records) {
		HistoryEntry entry;
		// timestamp filled in by HistoryManager::record()
		entry.action         = actionToString(record.action);
		entry.packageName    = record.packageName;
		entry.packageVersion = record.packageVersion;
		entry.system         = record.system;
		entry.status         = record.status;
		entry.errorMessage   = record.errorMessage;
		(void)this->historyManager->record(entry);
	}
}

std::vector<Package> Executer::orderedPackages(const Graph& graph) const {
	const auto numVertices = boost::num_vertices(graph);
	if (numVertices == 0) {
		return {};
	}

	std::vector<Graph::vertex_descriptor> order;
	boost::topological_sort(graph, std::back_inserter(order));
	std::reverse(order.begin(), order.end());

	// Compute the topological level for each vertex (longest path from any root).
	// Vertices at the same level have no dependency between them and can be freely
	// reordered.  This lets createTaskGroups() batch same-system packages together.
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

	// Within the same topological level sort by system name so that same-system
	// packages become consecutive and createTaskGroups() merges them into a single
	// TaskGroup (one plugin call instead of many).
	std::stable_sort(order.begin(), order.end(), [&](const Graph::vertex_descriptor a, const Graph::vertex_descriptor b) {
		if (levels[a] != levels[b]) {
			return levels[a] < levels[b];
		}
		return graph[a].system < graph[b].system;
	});

	std::vector<Package> packages;
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
	IPlugin* plugin = this->registry->getPlugin(taskGroup.system);
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
			Logger::instance().warn(finding.message);
		}
		if (finding.kind == "sync_error") {
			Logger::instance().err(finding.message);
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
