#include "core/execution/executor.h"

#include "executor_internal.h"

#include <vector>

std::vector<Executer::TransactionRecord> Executer::executeTaskGroups(const std::vector<TaskGroup>& taskGroups, const Graph* graph) const {
	return this->executeRecordedTaskGroups(taskGroups, this->activeRunId, graph);
}
