#include "core/executor.h"

#include <boost/graph/topological_sort.hpp>

#include <algorithm>
#include <iterator>
#include <map>

Executer::Executer(Registry* registry) {
	this->registry = registry;
}

Executer::~Executer() {}

void Executer::execute(Graph *graph) {
	if (graph == nullptr) {
		return;
	}

	this->startTransactionDb();
	if (!this->canWriteToVirtualFileSystem()) {
		return;
	}

	const std::vector<TaskGroup> taskGroups = this->createTaskGroups(*graph);
	const std::vector<TransactionRecord> records = this->executeTaskGroups(taskGroups);
	this->writeTransactionResults(records);
	this->markCommittedTransactions();
	this->deleteCommittedTransactions();
}

void Executer::startTransactionDb() const {
	// Skeleton hook: later this will open or initialize the transaction database.
}

bool Executer::canWriteToVirtualFileSystem() const {
	// Skeleton hook: later this will check VFS write permissions and locks.
	return true;
}

std::vector<Executer::TaskGroup> Executer::createTaskGroups(const Graph& graph) const {
	std::vector<TaskGroup> groups;
	std::map<std::string, std::size_t> groupIndexByKey;

	for (const Package& package : this->orderedPackages(graph)) {
		const std::string key = std::to_string(static_cast<int>(package.action)) + ":" + package.system;
		const auto [it, inserted] = groupIndexByKey.emplace(key, groups.size());
		if (inserted) {
			groups.push_back(TaskGroup{.action = package.action, .system = package.system});
		}

		groups[it->second].packages.push_back(package);
	}

	return groups;
}

std::vector<Executer::TransactionRecord> Executer::executeTaskGroups(const std::vector<TaskGroup>& taskGroups) const {
	std::vector<TransactionRecord> records;

	for (const TaskGroup& taskGroup : taskGroups) {
		const std::vector<TransactionRecord> groupRecords = this->executeTaskGroup(taskGroup);
		records.insert(records.end(), groupRecords.begin(), groupRecords.end());
	}

	return records;
}

std::vector<Executer::TransactionRecord> Executer::executeTaskGroup(const TaskGroup& taskGroup) const {
	if (taskGroup.packages.empty()) {
		return {};
	}

	if (this->registry->getPlugin(taskGroup.system) == nullptr || !this->registry->loadPlugin(taskGroup.system)) {
		return this->buildFailureRecords(taskGroup);
	}

	this->dispatchTaskGroupToPlugin(taskGroup);
	return this->buildSuccessRecords(taskGroup);
}

void Executer::writeTransactionResults(const std::vector<TransactionRecord>& records) const {
	(void)records;
	// Skeleton hook: later this will persist success and failure records to the transaction database.
}

void Executer::markCommittedTransactions() const {
	// Skeleton hook: later this will mark completed transaction records as committed.
}

void Executer::deleteCommittedTransactions() const {
	// Skeleton hook: later this will remove committed transaction records from the database.
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

void Executer::dispatchTaskGroupToPlugin(const TaskGroup& taskGroup) const {
	IPlugin* plugin = this->registry->getPlugin(taskGroup.system);
	if (plugin == nullptr) {
		return;
	}

	switch (taskGroup.action) {
		case ActionType::INSTALL:
			plugin->install(taskGroup.packages);
			break;
		case ActionType::REMOVE:
			plugin->remove(taskGroup.packages);
			break;
		case ActionType::UPDATE:
			plugin->update(taskGroup.packages);
			break;
		case ActionType::SEARCH:
		case ActionType::UNKNOWN:
		default:
			break;
	}
}

std::vector<Executer::TransactionRecord> Executer::buildSuccessRecords(const TaskGroup& taskGroup) const {
	std::vector<TransactionRecord> records;
	records.reserve(taskGroup.packages.size());

	for (const Package& package : taskGroup.packages) {
		records.push_back(TransactionRecord{
			.system = taskGroup.system,
			.packageName = package.name,
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
			.system = taskGroup.system,
			.packageName = package.name,
			.status = "failed"
		});
	}

	return records;
}
