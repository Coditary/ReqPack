#pragma once

#include "core/registry.h"
#include "core/types.h"

#include <map>
#include <string>
#include <vector>

class Executer {
	struct TransactionRecord {
		std::string system;
		std::string packageName;
		std::string status;
	};

	struct TaskGroup {
		ActionType action{ActionType::UNKNOWN};
		std::string system;
		std::vector<Package> packages;
	};

	Registry* registry;

	void startTransactionDb() const;
	bool canWriteToVirtualFileSystem() const;
	std::vector<TaskGroup> createTaskGroups(const Graph& graph) const;
	std::vector<TransactionRecord> executeTaskGroups(const std::vector<TaskGroup>& taskGroups) const;
	std::vector<TransactionRecord> executeTaskGroup(const TaskGroup& taskGroup) const;
	void writeTransactionResults(const std::vector<TransactionRecord>& records) const;
	void markCommittedTransactions() const;
	void deleteCommittedTransactions() const;
	std::vector<Package> orderedPackages(const Graph& graph) const;
	void dispatchTaskGroupToPlugin(const TaskGroup& taskGroup) const;
	std::vector<TransactionRecord> buildSuccessRecords(const TaskGroup& taskGroup) const;
	std::vector<TransactionRecord> buildFailureRecords(const TaskGroup& taskGroup) const;

public:
	Executer(Registry* registry);
	~Executer();

	void execute(Graph *graph);
};
