#pragma once

#include "core/configuration.h"
#include "core/registry.h"
#include "core/transaction_database.h"
#include "core/types.h"

#include <memory>
#include <map>
#include <string>
#include <vector>

class Executer {
	ReqPackConfig config;

	struct TransactionRecord {
		std::string runId;
		std::string system;
		ActionType action{ActionType::UNKNOWN};
		std::string packageName;
		std::string packageVersion;
		std::string status;
		std::string errorMessage;
	};

	struct TaskGroup {
		ActionType action{ActionType::UNKNOWN};
		std::string system;
		std::vector<Package> packages;
	};

	Registry* registry;
	std::unique_ptr<TransactionDatabase> transactionDatabase;
	mutable std::string activeRunId;

	void startTransactionDb() const;
	bool canWriteToVirtualFileSystem() const;
	std::vector<TaskGroup> recoverPendingTaskGroups() const;
	std::vector<TransactionItemRecord> reconcileRecoveredItems(const std::string& runId, const std::vector<TransactionItemRecord>& items) const;
	std::vector<TaskGroup> createTaskGroupsFromRecords(const std::vector<TransactionItemRecord>& records) const;
	std::vector<Package> collectPackages(const std::vector<TaskGroup>& taskGroups) const;
	std::vector<TaskGroup> createTaskGroups(const Graph& graph) const;
	std::vector<TaskGroup> filterExecutableTaskGroups(const std::vector<TaskGroup>& taskGroups) const;
	std::vector<TransactionRecord> executeTaskGroups(const std::vector<TaskGroup>& taskGroups) const;
	std::vector<TransactionRecord> executeRecordedTaskGroups(const std::vector<TaskGroup>& taskGroups, const std::string& runId) const;
	std::vector<TransactionRecord> executeTransactionalTaskGroup(const TaskGroup& taskGroup, const std::string& runId) const;
	std::vector<TransactionRecord> executeTaskGroup(const TaskGroup& taskGroup) const;
	std::vector<TransactionRecord> executeTaskGroup(const TaskGroup& taskGroup, const std::string& runId) const;
	void writeTransactionResults(const std::vector<TransactionRecord>& records) const;
	void markCommittedTransactions() const;
	void deleteCommittedTransactions() const;
	std::vector<Package> orderedPackages(const Graph& graph) const;
	bool dispatchTaskGroupToPlugin(const TaskGroup& taskGroup) const;
	std::vector<TransactionRecord> buildSuccessRecords(const TaskGroup& taskGroup) const;
	std::vector<TransactionRecord> buildFailureRecords(const TaskGroup& taskGroup) const;

public:
	Executer(Registry* registry, const ReqPackConfig& config = DEFAULT_REQPACK_CONFIG);
	~Executer();

	void execute(Graph *graph);
};
