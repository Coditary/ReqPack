#include "core/execution/executor.h"

#include "executor_internal.h"

#include "output/logger.h"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

struct ParallelUpdateState {
	std::mutex mutex;
	std::condition_variable condition;
	std::queue<std::size_t> readyIndices;
	std::set<std::string> busySystems;
	std::vector<bool> started;
	std::vector<bool> completed;
	std::vector<bool> failed;
	bool stopLaunching{false};
	std::size_t runningWorkers{0};
};

}  // namespace

bool Executer::updateSystem(const Request& request) const {
	const std::vector<bool> results = this->updateSystems({request});
	return !results.empty() && results.front();
}

std::vector<bool> Executer::updateSystems(const std::vector<Request>& requests) const {
	std::vector<bool> results(requests.size(), false);
	if (requests.empty()) {
		return results;
	}

	struct UpdateTask {
		std::size_t requestIndex{0};
		TaskGroup taskGroup;
	};

	std::vector<UpdateTask> updateTasks;
	updateTasks.reserve(requests.size());
	for (std::size_t index = 0; index < requests.size(); ++index) {
		const Request& request = requests[index];
		if (request.system.empty() || request.usesLocalTarget || !request.packages.empty()) {
			continue;
		}

		std::string errorMessage;
		const std::optional<Request> resolvedRequest = this->resolveRequest(request, &errorMessage);
		if (!resolvedRequest.has_value()) {
			if (!errorMessage.empty()) {
				Logger::instance().diagnostic(resolution_failure_diagnostic(errorMessage, "update"));
			}
			continue;
		}

		updateTasks.push_back(UpdateTask{
			.requestIndex = index,
			.taskGroup = TaskGroup{
				.action = ActionType::UPDATE,
				.system = resolvedRequest->system,
				.flags = resolvedRequest->flags,
			}
		});
	}

	if (updateTasks.empty()) {
		return results;
	}

	std::vector<TaskGroup> preloadGroups;
	preloadGroups.reserve(updateTasks.size());
	for (const UpdateTask& task : updateTasks) {
		preloadGroups.push_back(task.taskGroup);
	}
	this->preloadTaskGroups(preloadGroups);
	for (std::size_t index = 0; index < updateTasks.size(); ++index) {
		updateTasks[index].taskGroup = preloadGroups[index];
	}

	auto executeUpdate = [&](const TaskGroup& taskGroup) {
		if (this->securityGateway.isGatewaySystem(taskGroup.system)) {
			return this->dispatchTaskGroupToSecurityGateway(taskGroup);
		}
		if (taskGroup.pluginLoadFailed) {
			return false;
		}
		if (this->config.execution.dryRun) {
			return true;
		}
		return this->dispatchTaskGroupToPlugin(taskGroup);
	};

	auto runTask = [&](const UpdateTask& task) {
		Logger::instance().displayItemBegin(task.taskGroup.system, task.taskGroup.system);
		Logger::instance().displayItemStep(task.taskGroup.system, "update all packages");
		const bool ok = executeUpdate(task.taskGroup);
		if (ok) {
			Logger::instance().displayItemSuccess(task.taskGroup.system);
		} else {
			Logger::instance().displayItemFailure(task.taskGroup.system, system_update_failure_diagnostic(task.taskGroup.system));
		}
		results[task.requestIndex] = ok;
		return ok;
	};

	if (resolved_execution_jobs(this->config) <= 1 || updateTasks.size() <= 1) {
		for (const UpdateTask& task : updateTasks) {
			const bool ok = runTask(task);
			if (!ok && this->config.execution.stopOnFirstFailure) {
				break;
			}
		}
		return results;
	}

	ParallelUpdateState state;
	for (std::size_t index = 0; index < updateTasks.size(); ++index) {
		state.readyIndices.push(index);
	}
	state.started.assign(updateTasks.size(), false);
	state.completed.assign(updateTasks.size(), false);
	state.failed.assign(updateTasks.size(), false);

	auto worker = [&]() {
		for (;;) {
			std::size_t index = 0;
			UpdateTask task;
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
						if (state.started[candidate]) {
							continue;
						}
						const std::string& system = updateTasks[candidate].taskGroup.system;
						if (state.busySystems.contains(system)) {
							state.readyIndices.push(candidate);
							continue;
						}
						state.started[candidate] = true;
						state.busySystems.insert(system);
						++state.runningWorkers;
						index = candidate;
						task = updateTasks[candidate];
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

			const bool ok = runTask(task);
			{
				std::lock_guard<std::mutex> lock(state.mutex);
				state.completed[index] = true;
				state.failed[index] = !ok;
				state.busySystems.erase(task.taskGroup.system);
				if (!ok && this->config.execution.stopOnFirstFailure) {
					state.stopLaunching = true;
				}
				if (state.runningWorkers > 0) {
					--state.runningWorkers;
				}
			}
			state.condition.notify_all();
		}
	};

	const std::size_t workerCount = std::min<std::size_t>(resolved_execution_jobs(this->config), updateTasks.size());
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

	return results;
}
