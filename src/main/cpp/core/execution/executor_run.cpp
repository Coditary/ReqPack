#include "core/execution/executor.h"

#include "executor_internal.h"

#include "output/logger.h"

#include <algorithm>
#include <vector>

bool Executer::execute(Graph *graph) {
	if (graph == nullptr) {
		return false;
	}

	const int requestedItemCount = this->requestedItemCount;
	const bool inputAlreadyFiltered = this->inputAlreadyFiltered;
	this->requestedItemCount = 0;
	this->inputAlreadyFiltered = false;

	if (this->config.execution.useTransactionDb) {
		this->startTransactionDb();
		if (this->transactionDatabase == nullptr || !this->transactionDatabase->ensureReady()) {
			return false;
		}
	}

	if (this->config.execution.checkVirtualFileSystemWrite && !this->canWriteToVirtualFileSystem()) {
		return false;
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
			return false;
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
			return false;
		}
	}

	bool sessionBegun = false;
	if (!taskGroups.empty()) {
		std::vector<std::string> itemIds;
		for (const TaskGroup& tg : taskGroups) {
			if (tg.usesLocalTarget) {
				itemIds.push_back(tg.system + ":local");
			} else {
				for (const Package& pkg : tg.packages) {
					itemIds.push_back(package_item_id(tg.system, pkg));
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

	if (sessionBegun) {
		int succeeded = 0;
		int failed = 0;
		for (const TransactionRecord& r : allRecords) {
			if (r.status == "success") {
				++succeeded;
			} else {
				++failed;
			}
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

	return std::none_of(allRecords.begin(), allRecords.end(), [](const TransactionRecord& record) {
		return record.status != "success";
	});
}
