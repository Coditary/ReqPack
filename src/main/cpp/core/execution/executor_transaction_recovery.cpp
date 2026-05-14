#include "core/execution/executor.h"

#include "executor_internal.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

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
