#include "core/execution/executor.h"

#include "executor_internal.h"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

struct ParallelExecutionState {
	std::mutex mutex;
	std::condition_variable condition;
	std::queue<std::size_t> readyIndices;
	std::set<std::string> busySystems;
	std::vector<std::size_t> pendingDependencies;
	std::vector<bool> started;
	std::vector<bool> completed;
	std::vector<bool> failed;
	bool stopLaunching{false};
	std::size_t runningWorkers{0};
};

}  // namespace

std::vector<Executer::TransactionRecord> Executer::executeRecordedTaskGroups(const std::vector<TaskGroup>& taskGroups, const std::string& runId, const Graph* graph) const {
	std::vector<TaskGroupPlan> plans = this->createTaskGroupPlans(taskGroups, graph);

	auto has_failure = [](const std::vector<TransactionRecord>& groupRecords) {
		return std::any_of(groupRecords.begin(), groupRecords.end(), [](const TransactionRecord& record) {
			return record.status == "failed";
		});
	};

	auto run_sequential = [&]() {
		std::vector<TransactionRecord> records;
		std::vector<std::size_t> pendingDependencies;
		std::vector<bool> blocked(plans.size(), false);
		pendingDependencies.reserve(plans.size());
		for (const TaskGroupPlan& plan : plans) {
			pendingDependencies.push_back(plan.pendingDependencies);
		}

		for (std::size_t index = 0; index < plans.size(); ++index) {
			if (blocked[index] || pendingDependencies[index] != 0) {
				continue;
			}

			const std::vector<TransactionRecord> groupRecords = this->executeTaskGroup(plans[index].taskGroup, runId);
			records.insert(records.end(), groupRecords.begin(), groupRecords.end());
			const bool failed = has_failure(groupRecords);

			for (const std::size_t successor : plans[index].successors) {
				if (pendingDependencies[successor] > 0) {
					--pendingDependencies[successor];
				}
				if (failed) {
					blocked[successor] = true;
				}
			}

			if (failed && this->config.execution.stopOnFirstFailure) {
				break;
			}
		}

		return records;
	};

	if (graph == nullptr || resolved_execution_jobs(this->config) <= 1 || taskGroups.size() <= 1) {
		return run_sequential();
	}

	std::vector<std::vector<TransactionRecord>> resultSlots(plans.size());
	ParallelExecutionState state;
	state.pendingDependencies.reserve(plans.size());
	state.started.assign(plans.size(), false);
	state.completed.assign(plans.size(), false);
	state.failed.assign(plans.size(), false);
	for (const TaskGroupPlan& plan : plans) {
		state.pendingDependencies.push_back(plan.pendingDependencies);
	}
	for (std::size_t index = 0; index < plans.size(); ++index) {
		if (state.pendingDependencies[index] == 0) {
			state.readyIndices.push(index);
		}
	}

	auto worker = [&]() {
		for (;;) {
			std::size_t index = 0;
			TaskGroup taskGroup;
			{
				std::unique_lock<std::mutex> lock(state.mutex);
				for (;;) {
					if (state.stopLaunching || (state.readyIndices.empty() && state.runningWorkers == 0)) {
						return;
					}

					const std::size_t readyCount = state.readyIndices.size();
					bool found = false;
					for (std::size_t attempt = 0; attempt < readyCount; ++attempt) {
						const std::size_t candidate = state.readyIndices.front();
						state.readyIndices.pop();
						if (state.started[candidate] || state.failed[candidate] || state.pendingDependencies[candidate] != 0) {
							continue;
						}
						const std::string& system = plans[candidate].taskGroup.system;
						if (state.busySystems.contains(system)) {
							state.readyIndices.push(candidate);
							continue;
						}
						state.started[candidate] = true;
						state.busySystems.insert(system);
						++state.runningWorkers;
						index = candidate;
						taskGroup = plans[candidate].taskGroup;
						found = true;
						break;
					}

					if (found) {
						break;
					}
					if (state.runningWorkers == 0) {
						return;
					}
					state.condition.wait(lock);
				}
			}

			std::vector<TransactionRecord> groupRecords = this->executeTaskGroup(taskGroup, runId);
			const bool failed = has_failure(groupRecords);

			{
				std::lock_guard<std::mutex> lock(state.mutex);
				resultSlots[index] = std::move(groupRecords);
				state.completed[index] = true;
				state.failed[index] = failed;
				state.busySystems.erase(taskGroup.system);
				for (const std::size_t successor : plans[index].successors) {
					if (state.pendingDependencies[successor] > 0) {
						--state.pendingDependencies[successor];
					}
					if (failed) {
						state.failed[successor] = true;
					}
					if (state.pendingDependencies[successor] == 0 && !state.started[successor] && !state.completed[successor] && !state.failed[successor]) {
						state.readyIndices.push(successor);
					}
				}
				if (failed && this->config.execution.stopOnFirstFailure) {
					state.stopLaunching = true;
				}
				if (state.runningWorkers > 0) {
					--state.runningWorkers;
				}
			}
			state.condition.notify_all();
		}
	};

	const std::size_t workerCount = std::min<std::size_t>(resolved_execution_jobs(this->config), plans.size());
	std::vector<std::thread> workers;
	workers.reserve(workerCount);
	for (std::size_t index = 0; index < workerCount; ++index) {
		workers.emplace_back(worker);
	}
	for (std::thread& thread : workers) {
		if (thread.joinable()) {
			thread.join();
		}
	}

	std::vector<TransactionRecord> records;
	for (const auto& slot : resultSlots) {
		records.insert(records.end(), slot.begin(), slot.end());
	}
	return records;
}
