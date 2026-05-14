#include "core/execution/executor.h"

#include "executor_internal.h"

#include <algorithm>
#include <optional>
#include <vector>

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
	// Any failed or still-incomplete item means run must stay active as "failed"
	// so callers can inspect it and recovery logic can distinguish it from committed run.
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
