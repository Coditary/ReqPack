#include "core/executor.h"

#include <boost/graph/topological_sort.hpp>

#include <algorithm>
#include <iterator>

namespace {

bool samePackage(const Package& left, const Package& right) {
	return left.action == right.action && left.system == right.system && left.name == right.name && left.version == right.version;
}

bool actionUsesDesiredStateFilter(const ActionType action) {
	return action == ActionType::INSTALL || action == ActionType::ENSURE || action == ActionType::REMOVE || action == ActionType::UPDATE;
}

bool actionSupportsRecoveryReconciliation(const ActionType action) {
	return action == ActionType::INSTALL || action == ActionType::ENSURE || action == ActionType::REMOVE || action == ActionType::UPDATE;
}

}  // namespace

Executer::Executer(Registry* registry, const ReqPackConfig& config) : config(config) {
	this->registry = registry;
	this->transactionDatabase = std::make_unique<TransactionDatabase>(config);
}

Executer::~Executer() {}

void Executer::execute(Graph *graph) {
	if (graph == nullptr) {
		return;
	}

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
		const bool hadActiveRun = this->transactionDatabase->getActiveRun().has_value();
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
		if (hadActiveRun) {
			return;
		}
		if (this->transactionDatabase->getActiveRun().has_value()) {
			return;
		}
		this->activeRunId.clear();
	}
	const std::vector<TaskGroup> plannedTaskGroups = this->filterExecutableTaskGroups(this->createTaskGroups(*graph));
	taskGroups = plannedTaskGroups;
	if (this->config.execution.useTransactionDb && !taskGroups.empty()) {
		this->activeRunId.clear();
		this->activeRunId = this->transactionDatabase->createRun(this->collectPackages(taskGroups));
		if (this->activeRunId.empty()) {
			return;
		}
	}
	const std::vector<TransactionRecord> records = this->executeTaskGroups(taskGroups);
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
		if (item.status == "success" || item.status == "committed") {
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
	return this->createTaskGroupsFromRecords(pendingItems);
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
			groups.push_back(TaskGroup{.action = record.package.action, .system = record.package.system});
		}
		groups.back().packages.push_back(record.package);
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
			groups.push_back(TaskGroup{.action = package.action, .system = package.system});
		}

		groups.back().packages.push_back(package);
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
		if (plugin == nullptr || !this->registry->loadPlugin(taskGroup.system)) {
			filteredTaskGroups.push_back(taskGroup);
			continue;
		}

		TaskGroup filteredTaskGroup = taskGroup;
		filteredTaskGroup.packages = plugin->getMissingPackages(taskGroup.packages);
		if (!filteredTaskGroup.packages.empty()) {
			filteredTaskGroups.push_back(std::move(filteredTaskGroup));
		}
	}

	return filteredTaskGroups;
}

std::vector<Executer::TransactionRecord> Executer::executeTaskGroups(const std::vector<TaskGroup>& taskGroups) const {
	return this->executeRecordedTaskGroups(taskGroups, this->activeRunId);
}

std::vector<Executer::TransactionRecord> Executer::executeRecordedTaskGroups(const std::vector<TaskGroup>& taskGroups, const std::string& runId) const {
	std::vector<TransactionRecord> records;

	for (const TaskGroup& taskGroup : taskGroups) {
		const std::vector<TransactionRecord> groupRecords = this->executeTaskGroup(taskGroup, runId);
		records.insert(records.end(), groupRecords.begin(), groupRecords.end());

		if (this->config.execution.stopOnFirstFailure) {
			const bool hasFailure = std::any_of(groupRecords.begin(), groupRecords.end(), [](const TransactionRecord& record) {
				return record.status == "failed";
			});

			if (hasFailure) {
				break;
			}
		}
	}

	return records;
}

std::vector<Executer::TransactionRecord> Executer::executeTaskGroup(const TaskGroup& taskGroup) const {
	return this->executeTaskGroup(taskGroup, this->activeRunId);
}

std::vector<Executer::TransactionRecord> Executer::executeTaskGroup(const TaskGroup& taskGroup, const std::string& runId) const {
	if (taskGroup.packages.empty()) {
		return {};
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

	TaskGroup executableTaskGroup = taskGroup;
	if (actionUsesDesiredStateFilter(taskGroup.action)) {
		IPlugin* plugin = this->registry->getPlugin(taskGroup.system);
		if (plugin != nullptr) {
			executableTaskGroup.packages = plugin->getMissingPackages(taskGroup.packages);
			if (executableTaskGroup.packages.empty()) {
				std::vector<TransactionRecord> records = this->buildSuccessRecords(taskGroup);
				for (TransactionRecord& record : records) {
					record.runId = runId;
				}
				return records;
			}
		}
	}

	if (!this->dispatchTaskGroupToPlugin(executableTaskGroup)) {
		std::vector<TransactionRecord> records = this->buildFailureRecords(executableTaskGroup);
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
	std::vector<TransactionRecord> records;
	records.reserve(taskGroup.packages.size());

	for (const Package& package : taskGroup.packages) {
		if (actionUsesDesiredStateFilter(taskGroup.action)) {
			IPlugin* plugin = this->registry->getPlugin(taskGroup.system);
			if (plugin != nullptr && plugin->getMissingPackages({package}).empty()) {
				std::vector<TransactionRecord> successRecords = this->buildSuccessRecords(TaskGroup{.action = taskGroup.action, .system = taskGroup.system, .packages = {package}});
				for (TransactionRecord& record : successRecords) {
					record.runId = runId;
				}
				records.insert(records.end(), successRecords.begin(), successRecords.end());
				continue;
			}
		}

		if (!this->transactionDatabase->updateItemStatus(runId, package, "running")) {
			std::vector<TransactionRecord> failureRecords = this->buildFailureRecords(TaskGroup{.action = taskGroup.action, .system = taskGroup.system, .packages = {package}});
			for (TransactionRecord& record : failureRecords) {
				record.runId = runId;
				record.errorMessage = "transaction update failed";
			}
			records.insert(records.end(), failureRecords.begin(), failureRecords.end());
			break;
		}

		const TaskGroup singlePackageTaskGroup{.action = taskGroup.action, .system = taskGroup.system, .packages = {package}};
		if (!this->dispatchTaskGroupToPlugin(singlePackageTaskGroup)) {
			std::vector<TransactionRecord> failureRecords = this->buildFailureRecords(singlePackageTaskGroup);
			for (TransactionRecord& record : failureRecords) {
				record.runId = runId;
				record.errorMessage = "plugin action failed";
			}
			records.insert(records.end(), failureRecords.begin(), failureRecords.end());
			if (this->config.execution.stopOnFirstFailure) {
				break;
			}
			continue;
		}

		std::vector<TransactionRecord> successRecords = this->buildSuccessRecords(singlePackageTaskGroup);
		for (TransactionRecord& record : successRecords) {
			record.runId = runId;
		}
		records.insert(records.end(), successRecords.begin(), successRecords.end());
	}

	return records;
}

void Executer::writeTransactionResults(const std::vector<TransactionRecord>& records) const {
	if (this->transactionDatabase == nullptr || this->activeRunId.empty()) {
		return;
	}

	for (const TransactionRecord& record : records) {
		Package package;
		package.action = record.action;
		package.system = record.system;
		package.name = record.packageName;
		package.version = record.packageVersion;
		(void)this->transactionDatabase->updateItemStatus(this->activeRunId, package, record.status, record.errorMessage);
	}
}

void Executer::markCommittedTransactions() const {
	if (this->transactionDatabase == nullptr || this->activeRunId.empty()) {
		return;
	}

	const std::vector<TransactionItemRecord> items = this->transactionDatabase->getRunItems(this->activeRunId);
	const bool hasIncompleteItem = std::any_of(items.begin(), items.end(), [](const TransactionItemRecord& item) {
		return item.status != "success" && item.status != "committed";
	});
	if (hasIncompleteItem) {
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

std::vector<Package> Executer::orderedPackages(const Graph& graph) const {
	std::vector<Graph::vertex_descriptor> order;
	boost::topological_sort(graph, std::back_inserter(order));
	std::reverse(order.begin(), order.end());

	std::vector<Package> packages;
	packages.reserve(order.size());
	for (const Graph::vertex_descriptor vertex : order) {
		packages.push_back(graph[vertex]);
	}

	return packages;
}

bool Executer::dispatchTaskGroupToPlugin(const TaskGroup& taskGroup) const {
	IPlugin* plugin = this->registry->getPlugin(taskGroup.system);
	if (plugin == nullptr) {
		return false;
	}

		switch (taskGroup.action) {
			case ActionType::INSTALL:
			case ActionType::ENSURE:
				return plugin->install(taskGroup.packages);
		case ActionType::REMOVE:
			return plugin->remove(taskGroup.packages);
		case ActionType::UPDATE:
			return plugin->update(taskGroup.packages);
		case ActionType::SEARCH:
		case ActionType::UNKNOWN:
		default:
			return false;
	}
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
