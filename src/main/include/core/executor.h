#pragma once

#include "core/configuration.h"
#include "core/history_manager.h"
#include "core/registry.h"
#include "core/security_gateway_service.h"
#include "core/transaction_database.h"
#include "core/types.h"

#include <memory>
#include <map>
#include <set>
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
		std::vector<std::string> flags;
		std::string localPath;
		bool usesLocalTarget{false};
	};

	Registry* registry;
	SecurityGatewayService securityGateway;
	std::unique_ptr<TransactionDatabase> transactionDatabase;
	std::unique_ptr<HistoryManager> historyManager;
	mutable std::string activeRunId;
	mutable int requestedItemCount{0};
	mutable bool inputAlreadyFiltered{false};

	void startTransactionDb() const;
	bool canWriteToVirtualFileSystem() const;
	std::vector<TaskGroup> recoverPendingTaskGroups() const;
	std::vector<TransactionItemRecord> reconcileRecoveredItems(const std::string& runId, const std::vector<TransactionItemRecord>& items) const;
	std::vector<TaskGroup> createTaskGroupsFromRecords(const std::vector<TransactionItemRecord>& records) const;
	std::vector<Package> collectPackages(const std::vector<TaskGroup>& taskGroups) const;
	std::vector<TaskGroup> createTaskGroups(const Graph& graph) const;
	std::vector<TaskGroup> filterExecutableTaskGroups(const std::vector<TaskGroup>& taskGroups) const;
	std::vector<TransactionRecord> executeTaskGroups(const std::vector<TaskGroup>& taskGroups, const Graph* graph = nullptr) const;
	std::vector<TransactionRecord> executeRecordedTaskGroups(const std::vector<TaskGroup>& taskGroups, const std::string& runId, const Graph* graph = nullptr) const;
	std::vector<TransactionRecord> executeTransactionalTaskGroup(const TaskGroup& taskGroup, const std::string& runId) const;
	std::vector<TransactionRecord> executeTaskGroup(const TaskGroup& taskGroup) const;
	std::vector<TransactionRecord> executeTaskGroup(const TaskGroup& taskGroup, const std::string& runId) const;
	PluginCallContext buildPluginContext(IPlugin* plugin, const TaskGroup& taskGroup) const;
	bool dispatchTaskGroupToSecurityGateway(const TaskGroup& taskGroup) const;
	void writeTransactionResults(const std::vector<TransactionRecord>& records) const;
	void markCommittedTransactions() const;
	void deleteCommittedTransactions() const;
	std::optional<Request> resolveRequest(const Request& request, std::string* errorMessage = nullptr) const;
	bool syncInstalledStateForSystem(const std::string& system, bool allowEmpty) const;
	std::set<std::string> refreshInstalledState(const std::vector<TransactionRecord>& records) const;
	void recordHistory(const std::vector<TransactionRecord>& records) const;
	std::vector<Package> orderedPackages(const Graph& graph) const;
	bool dispatchTaskGroupToPlugin(const TaskGroup& taskGroup) const;
	std::vector<TransactionRecord> buildSuccessRecords(const TaskGroup& taskGroup) const;
	std::vector<TransactionRecord> buildFailureRecords(const TaskGroup& taskGroup) const;

public:
	Executer(Registry* registry, const ReqPackConfig& config = default_reqpack_config());
	~Executer();

	void setRequestedItemCount(int count, bool inputAlreadyFiltered = false) const;
	void execute(Graph *graph);
	std::vector<PackageInfo> list(const Request& request) const;
	std::vector<PackageInfo> outdated(const Request& request) const;
	std::vector<PackageInfo> search(const Request& request) const;
	PackageInfo info(const Request& request) const;
	std::optional<Package> resolvePackage(const Request& request, const Package& package) const;
};
