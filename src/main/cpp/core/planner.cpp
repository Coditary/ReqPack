#include "core/planner.h"

#include <boost/graph/topological_sort.hpp>

#include <algorithm>
#include <functional>
#include <utility>

namespace {

bool samePackage(const Package& left, const Package& right) {
	return left.action == right.action && left.system == right.system && left.name == right.name && left.version == right.version;
}

Package normalizeDependency(Package dependency, const std::string& defaultSystem) {
	if (dependency.action == ActionType::UNKNOWN) {
		dependency.action = ActionType::INSTALL;
	}

	if (dependency.system.empty()) {
		dependency.system = defaultSystem;
	}

	return dependency;
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

Planner::Planner(Registry* registry, RegistryDatabase* database, const ReqPackConfig& config) : config(config), downloader(database, config) {
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
	return this->registry->getPlugin(system) != nullptr;
}

void Planner::queuePluginDownload(const std::string& system) const {
	if (!this->downloader.downloadPlugin(system)) {
		return;
	}

	this->registry->scanDirectory(this->config.registry.pluginDirectory);
}

std::vector<Package> Planner::collectPluginDependencies(const std::vector<Request>& requests) const {
	std::vector<Package> dependencies;

	for (const Request& request : requests) {
		IPlugin* plugin = this->registry->getPlugin(request.system);
		if (plugin == nullptr) {
			continue;
		}

		for (Package dependency : plugin->getRequirements()) {
			dependencies.push_back(normalizeDependency(std::move(dependency), request.system));
		}
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
	return !dependency.system.empty() && this->registry->getPlugin(dependency.system) != nullptr;
}

void Planner::queueDependencyDownload(const Package& dependency) const {
	if (dependency.system.empty()) {
		return;
	}

	if (!this->downloader.downloadPlugin(dependency.system)) {
		return;
	}

	this->registry->scanDirectory(this->config.registry.pluginDirectory);
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

	std::function<Graph::vertex_descriptor(const Package&, std::vector<std::string>&)> addPackageWithDependencies;
	addPackageWithDependencies = [&](const Package& package, std::vector<std::string>& activeSystems) -> Graph::vertex_descriptor {
		const Graph::vertex_descriptor packageVertex = findOrAddPackageVertex(graph, package);

		if (std::find(activeSystems.begin(), activeSystems.end(), package.system) != activeSystems.end()) {
			return packageVertex;
		}

		activeSystems.push_back(package.system);

		IPlugin* plugin = this->registry->getPlugin(package.system);
		if (plugin == nullptr && this->config.planner.autoDownloadMissingDependencies) {
			this->queueDependencyDownload(Package{.action = ActionType::INSTALL, .system = package.system});
			plugin = this->registry->getPlugin(package.system);
		}

		if (plugin != nullptr) {
			for (Package dependency : plugin->getRequirements()) {
				const Package normalizedDependency = normalizeDependency(std::move(dependency), package.system);
				const Graph::vertex_descriptor dependencyVertex = addPackageWithDependencies(normalizedDependency, activeSystems);
				if (dependencyVertex != packageVertex && !boost::edge(dependencyVertex, packageVertex, graph).second) {
					boost::add_edge(dependencyVertex, packageVertex, graph);
				}
			}
		}

		activeSystems.pop_back();
		return packageVertex;
	};

	for (const std::string& packageName : request.packages) {
		const Package package = this->makeRequestedPackage(request, packageName);
		std::vector<std::string> activeSystems;
		(void)addPackageWithDependencies(package, activeSystems);
	}
}

Package Planner::makeRequestedPackage(const Request& request, const std::string& packageSpecifier) const {
	Package package;
	package.action = request.action;
	package.system = request.system;

	const std::size_t versionSeparator = packageSpecifier.rfind('@');
	if (versionSeparator == std::string::npos || versionSeparator == 0 || versionSeparator == packageSpecifier.size() - 1) {
		package.name = packageSpecifier;
		return package;
	}

	package.name = packageSpecifier.substr(0, versionSeparator);
	package.version = packageSpecifier.substr(versionSeparator + 1);
	return package;
}

std::vector<Graph::vertex_descriptor> Planner::topologicallySort(const Graph& graph) const {
	std::vector<Graph::vertex_descriptor> order;
	boost::topological_sort(graph, std::back_inserter(order));
	std::reverse(order.begin(), order.end());
	return order;
}
