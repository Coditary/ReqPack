#include "core/planner.h"

#include <boost/graph/topological_sort.hpp>

#include <algorithm>
#include <utility>

namespace {

bool samePackage(const Package& left, const Package& right) {
	return left.action == right.action && left.system == right.system && left.name == right.name && left.version == right.version;
}

Graph::vertex_descriptor findOrAddPackageVertex(Graph& graph, const Package& package) {
	auto [vertex, vertexEnd] = boost::vertices(graph);
	for (; vertex != vertexEnd; ++vertex) {
		if (samePackage(graph[*vertex], package)) {
			return *vertex;
		}
	}

	return boost::add_vertex(package, graph);
}

}  // namespace

Planner::Planner(Registry* registry, const ReqPackConfig& config) : config(config) {
	this->registry = registry;
}

Planner::~Planner() {}

Graph* Planner::plan(const std::vector<Request>& requests) {
	const std::vector<Request> expandedRequests = this->config.planner.enableProxyExpansion ?
		this->expandProxies(requests) :
		requests;
	this->ensurePluginsAvailable(expandedRequests);

	const std::vector<Package> dependencies = this->collectPluginDependencies(expandedRequests);
	this->ensurePluginDependenciesAvailable(dependencies);

	Graph* graph = this->buildDag(expandedRequests);
	if (this->config.planner.topologicallySortGraph) {
		(void)this->topologicallySort(*graph);
	}

	return graph;
}

std::vector<Request> Planner::expandProxies(const std::vector<Request>& requests) const {
	std::vector<Request> expandedRequests = requests;

	for (Request& request : expandedRequests) {
		auto alias = this->config.planner.systemAliases.find(request.system);
		if (alias == this->config.planner.systemAliases.end()) {
			continue;
		}

		request.system = alias->second;
	}

	return expandedRequests;
}

void Planner::ensurePluginsAvailable(const std::vector<Request>& requests) const {
	if (!this->config.planner.autoDownloadMissingPlugins) {
		return;
	}

	for (const Request& request : requests) {
		if (this->pluginExists(request.system)) {
			continue;
		}

		this->queuePluginDownload(request.system);
	}
}

bool Planner::pluginExists(const std::string& system) const {
	if (this->registry->getPlugin(system) == nullptr) {
		return false;
	}

	return this->registry->loadPlugin(system);
}

void Planner::queuePluginDownload(const std::string& system) const {
	(void)system;
	// Skeleton hook: later this will enqueue a downloader request for a missing plugin.
}

std::vector<Package> Planner::collectPluginDependencies(const std::vector<Request>& requests) const {
	std::vector<Package> dependencies;

	for (const Request& request : requests) {
		IPlugin* plugin = this->registry->getPlugin(request.system);
		if (plugin == nullptr) {
			continue;
		}

		const std::vector<Package> pluginDependencies = plugin->getRequirements();
		dependencies.insert(dependencies.end(), pluginDependencies.begin(), pluginDependencies.end());
	}

	return dependencies;
}

void Planner::ensurePluginDependenciesAvailable(const std::vector<Package>& dependencies) const {
	if (!this->config.planner.autoDownloadMissingDependencies) {
		return;
	}

	for (const Package& dependency : dependencies) {
		if (this->dependencyExists(dependency)) {
			continue;
		}

		this->queueDependencyDownload(dependency);
	}
}

bool Planner::dependencyExists(const Package& dependency) const {
	(void)dependency;
	// Skeleton hook: later this will inspect whether a dependency is already available.
	return true;
}

void Planner::queueDependencyDownload(const Package& dependency) const {
	(void)dependency;
	// Skeleton hook: later this will enqueue a downloader request for a missing dependency.
}

Graph* Planner::buildDag(const std::vector<Request>& requests) const {
	Graph* graph = new Graph();

	for (const Request& request : requests) {
		this->addRequestToGraph(*graph, request);
	}

	return graph;
}

void Planner::addRequestToGraph(Graph& graph, const Request& request) const {
	if (request.packages.empty()) {
		return;
	}

	std::vector<Graph::vertex_descriptor> dependencyVertices;
	IPlugin* plugin = this->registry->getPlugin(request.system);
	if (plugin != nullptr) {
		const std::vector<Package> dependencies = plugin->getRequirements();
		dependencyVertices.reserve(dependencies.size());

		for (const Package& dependency : dependencies) {
			dependencyVertices.push_back(findOrAddPackageVertex(graph, dependency));
		}
	}

	for (const std::string& packageName : request.packages) {
		Package package;
		package.action = request.action;
		package.system = request.system;
		package.name = packageName;

		const Graph::vertex_descriptor packageVertex = findOrAddPackageVertex(graph, package);
		for (const Graph::vertex_descriptor dependencyVertex : dependencyVertices) {
			if (dependencyVertex == packageVertex) {
				continue;
			}

			if (!boost::edge(dependencyVertex, packageVertex, graph).second) {
				boost::add_edge(dependencyVertex, packageVertex, graph);
			}
		}
	}
}

std::vector<Graph::vertex_descriptor> Planner::topologicallySort(const Graph& graph) const {
	std::vector<Graph::vertex_descriptor> order;
	boost::topological_sort(graph, std::back_inserter(order));
	std::reverse(order.begin(), order.end());
	return order;
}
