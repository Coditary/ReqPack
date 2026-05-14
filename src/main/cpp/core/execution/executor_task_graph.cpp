#include "core/execution/executor.h"

#include "executor_internal.h"

#include <boost/graph/topological_sort.hpp>

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <vector>

std::vector<Package> Executer::collectPackages(const std::vector<TaskGroup>& taskGroups) const {
	std::vector<Package> packages;
	for (const TaskGroup& taskGroup : taskGroups) {
		packages.insert(packages.end(), taskGroup.packages.begin(), taskGroup.packages.end());
	}
	return packages;
}

std::vector<Graph::vertex_descriptor> Executer::orderedVertices(const Graph& graph) const {
	const auto numVertices = boost::num_vertices(graph);
	if (numVertices == 0) {
		return {};
	}

	std::vector<Graph::vertex_descriptor> order;
	boost::topological_sort(graph, std::back_inserter(order));
	std::reverse(order.begin(), order.end());

	std::vector<int> levels(numVertices, 0);
	for (const Graph::vertex_descriptor v : order) {
		auto [edgeBegin, edgeEnd] = boost::out_edges(v, graph);
		for (auto edgeIt = edgeBegin; edgeIt != edgeEnd; ++edgeIt) {
			const Graph::vertex_descriptor target = boost::target(*edgeIt, graph);
			if (levels[target] <= levels[v]) {
				levels[target] = levels[v] + 1;
			}
		}
	}

	std::stable_sort(order.begin(), order.end(), [&](const Graph::vertex_descriptor a, const Graph::vertex_descriptor b) {
		if (levels[a] != levels[b]) {
			return levels[a] < levels[b];
		}
		const std::optional<std::size_t> ensureOrderA = internal_ensure_order(graph[a]);
		const std::optional<std::size_t> ensureOrderB = internal_ensure_order(graph[b]);
		if (ensureOrderA.has_value() || ensureOrderB.has_value()) {
			if (ensureOrderA.has_value() != ensureOrderB.has_value()) {
				return ensureOrderA.has_value();
			}
			if (ensureOrderA.value() != ensureOrderB.value()) {
				return ensureOrderA.value() < ensureOrderB.value();
			}
		}
		return graph[a].system < graph[b].system;
	});

	return order;
}

std::vector<Executer::TaskGroup> Executer::createTaskGroups(const Graph& graph) const {
	std::vector<TaskGroup> groups;

	for (const Graph::vertex_descriptor vertex : this->orderedVertices(graph)) {
		const Package& package = graph[vertex];
		if (groups.empty() || groups.back().action != package.action || groups.back().system != package.system) {
			groups.push_back(TaskGroup{.action = package.action, .system = package.system, .flags = package.flags});
		}

		groups.back().packages.push_back(package);
		if (package.localTarget) {
			groups.back().usesLocalTarget = true;
			groups.back().localPath = package.sourcePath;
		}
	}

	return groups;
}

void Executer::preloadTaskGroups(std::vector<TaskGroup>& taskGroups) const {
	for (TaskGroup& taskGroup : taskGroups) {
		taskGroup.plugin = nullptr;
		taskGroup.pluginLoadFailed = false;
		if (this->securityGateway.isGatewaySystem(taskGroup.system)) {
			continue;
		}
		if (this->registry->getPlugin(taskGroup.system) == nullptr || !this->registry->loadPlugin(taskGroup.system)) {
			taskGroup.pluginLoadFailed = true;
			continue;
		}
		taskGroup.plugin = this->registry->getPlugin(taskGroup.system);
		if (taskGroup.plugin == nullptr) {
			taskGroup.pluginLoadFailed = true;
		}
	}
}

std::vector<Executer::TaskGroupPlan> Executer::createTaskGroupPlans(const std::vector<TaskGroup>& taskGroups, const Graph* graph) const {
	std::vector<TaskGroupPlan> plans;
	plans.reserve(taskGroups.size());
	for (const TaskGroup& taskGroup : taskGroups) {
		plans.push_back(TaskGroupPlan{.taskGroup = taskGroup});
	}
	if (graph == nullptr || taskGroups.empty()) {
		return plans;
	}

	std::map<std::string, std::size_t> packageToGroupIndex;
	for (std::size_t groupIndex = 0; groupIndex < taskGroups.size(); ++groupIndex) {
		for (const Package& package : taskGroups[groupIndex].packages) {
			packageToGroupIndex[package_identity_key(package)] = groupIndex;
		}
	}

	std::set<std::pair<std::size_t, std::size_t>> seenEdges;
	auto [edgeBegin, edgeEnd] = boost::edges(*graph);
	for (auto edgeIt = edgeBegin; edgeIt != edgeEnd; ++edgeIt) {
		const Package& sourcePackage = (*graph)[boost::source(*edgeIt, *graph)];
		const Package& targetPackage = (*graph)[boost::target(*edgeIt, *graph)];
		const auto sourceGroupIt = packageToGroupIndex.find(package_identity_key(sourcePackage));
		const auto targetGroupIt = packageToGroupIndex.find(package_identity_key(targetPackage));
		if (sourceGroupIt == packageToGroupIndex.end() || targetGroupIt == packageToGroupIndex.end()) {
			continue;
		}
		if (sourceGroupIt->second == targetGroupIt->second) {
			continue;
		}
		const std::pair<std::size_t, std::size_t> edge{sourceGroupIt->second, targetGroupIt->second};
		if (!seenEdges.insert(edge).second) {
			continue;
		}
		plans[sourceGroupIt->second].successors.push_back(targetGroupIt->second);
		++plans[targetGroupIt->second].pendingDependencies;
	}

	return plans;
}

std::vector<Executer::TaskGroup> Executer::filterExecutableTaskGroups(const std::vector<TaskGroup>& taskGroups) const {
	std::vector<TaskGroup> filteredTaskGroups;

	for (const TaskGroup& taskGroup : taskGroups) {
		if (taskGroup.packages.empty()) {
			continue;
		}

		if (!actionUsesMissingPackageFilter(taskGroup.action)) {
			filteredTaskGroups.push_back(taskGroup);
			continue;
		}

		IPlugin* plugin = this->registry->getPlugin(taskGroup.system);
		if (this->securityGateway.isGatewaySystem(taskGroup.system)) {
			filteredTaskGroups.push_back(taskGroup);
			continue;
		}
		if (plugin == nullptr || !this->registry->loadPlugin(taskGroup.system)) {
			filteredTaskGroups.push_back(taskGroup);
			continue;
		}

		TaskGroup filteredTaskGroup = taskGroup;
		if (filteredTaskGroup.usesLocalTarget) {
			filteredTaskGroups.push_back(std::move(filteredTaskGroup));
			continue;
		}
		filteredTaskGroup.packages = plugin->getMissingPackages(taskGroup.packages);
		if (!filteredTaskGroup.packages.empty()) {
			filteredTaskGroups.push_back(std::move(filteredTaskGroup));
		}
	}

	return filteredTaskGroups;
}
