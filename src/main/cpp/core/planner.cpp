#include "core/planner.h"

#include <boost/graph/topological_sort.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <utility>

namespace {

std::filesystem::path requirementsMarkerPath(const ReqPackConfig& config, const std::string& system) {
	return std::filesystem::path(config.registry.pluginDirectory) / system / ".requirements_ready";
}

Package resolveDependencySystem(Package dependency, const Registry* registry) {
	if (dependency.system.empty()) {
		return dependency;
	}

	dependency.system = registry->resolvePluginName(dependency.system);
	return dependency;
}

Graph::vertex_descriptor findOrAddPackageVertex(Graph& graph, const Package& package) {
	auto [vertex, vertexEnd] = boost::vertices(graph);
	for (; vertex != vertexEnd; ++vertex) {
		if (planner_same_package(graph[*vertex], package)) {
			return *vertex;
		}
	}

	return boost::add_vertex(package, graph);
}

}  // namespace

Planner::Planner(Registry* registry, RegistryDatabase* database, const ReqPackConfig& config)
    : config(config), downloader(database, config), registry(registry), securityGateway(registry, registry, config) {
	this->registry = registry;
}

Planner::~Planner() {}

Graph* Planner::plan(const std::vector<Request>& requests) {
	const std::vector<Request> expandedRequests = this->config.planner.enableProxyExpansion ?
		this->expandProxies(requests) :
		requests;
	this->ensurePluginsAvailable(expandedRequests);
	const std::vector<Request> filteredRequests = this->filterRequestedPackages(expandedRequests);

	Graph* graph = nullptr;
	if (planner_contains_only_action(filteredRequests, ActionType::ENSURE)) {
		const std::vector<Package> dependencies = this->collectPluginDependencies(filteredRequests);
		this->ensurePluginDependenciesAvailable(dependencies);
		graph = this->buildDependencyDag(this->filterMissingDependencies(dependencies));
	} else {
		graph = this->buildDag(filteredRequests);
	}
	if (this->config.planner.topologicallySortGraph) {
		(void)this->topologicallySort(*graph);
	}

	return graph;
}

std::vector<Request> Planner::expandProxies(const std::vector<Request>& requests) const {
	return planner_expand_proxies(requests, this->config.planner.systemAliases);
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
	if (this->gatewayExists(system)) {
		return true;
	}
	return this->registry->getPlugin(system) != nullptr;
}

bool Planner::gatewayExists(const std::string& system) const {
	return this->securityGateway.isGatewaySystem(system);
}

void Planner::queuePluginDownload(const std::string& system) const {
	if (!this->downloader.downloadPlugin(system)) {
		return;
	}

	this->registry->scanDirectory(this->config.registry.pluginDirectory);
}

bool Planner::shouldInstallPluginDependencies(const std::string& system) const {
	const std::string resolvedSystem = this->registry->resolvePluginName(system);
	if (this->pluginRequirementsProvisioned(resolvedSystem)) {
		return false;
	}

	if (this->pluginRequirementsSatisfied(resolvedSystem)) {
		this->markPluginRequirementsProvisioned(resolvedSystem);
		return false;
	}

	return true;
}

bool Planner::pluginRequirementsSatisfied(const std::string& system) const {
	IPlugin* plugin = this->registry->getPlugin(system);
	if (plugin == nullptr || !this->registry->loadPlugin(system)) {
		return false;
	}

	std::vector<Package> requirements;
	for (Package dependency : plugin->getRequirements()) {
		requirements.push_back(resolveDependencySystem(planner_normalize_dependency(std::move(dependency), system), this->registry));
	}

	return this->filterMissingDependencies(requirements).empty();
}

bool Planner::pluginRequirementsProvisioned(const std::string& system) const {
	return std::filesystem::exists(requirementsMarkerPath(this->config, system));
}

void Planner::markPluginRequirementsProvisioned(const std::string& system) const {
	const std::filesystem::path markerPath = requirementsMarkerPath(this->config, system);
	std::error_code directoryError;
	std::filesystem::create_directories(markerPath.parent_path(), directoryError);
	if (directoryError) {
		return;
	}

	std::ofstream marker(markerPath, std::ios::binary | std::ios::trunc);
	if (!marker) {
		return;
	}

	marker << "ready\n";
}

std::vector<Request> Planner::filterRequestedPackages(const std::vector<Request>& requests) const {
	std::vector<Request> filteredRequests;
	filteredRequests.reserve(requests.size());

	for (const Request& request : requests) {
		if (request.action != ActionType::INSTALL || request.packages.empty()) {
			if (request.action == ActionType::INSTALL && request.usesLocalTarget) {
				filteredRequests.push_back(request);
				continue;
			}
			filteredRequests.push_back(request);
			continue;
		}

		IPlugin* plugin = this->registry->getPlugin(request.system);
		if (this->gatewayExists(request.system)) {
			filteredRequests.push_back(request);
			continue;
		}
		if (plugin == nullptr || !this->registry->loadPlugin(request.system)) {
			filteredRequests.push_back(request);
			continue;
		}

		std::vector<Package> requestedPackages;
		requestedPackages.reserve(request.packages.size());
		for (const std::string& packageSpecifier : request.packages) {
			requestedPackages.push_back(this->makeRequestedPackage(request, packageSpecifier));
		}

		const std::vector<Package> missingPackages = plugin->getMissingPackages(requestedPackages);
		const std::optional<Request> filteredRequest = planner_filter_install_request(request, missingPackages);
		if (!filteredRequest.has_value()) {
			continue;
		}

		filteredRequests.push_back(filteredRequest.value());
	}

	return filteredRequests;
}

std::vector<Package> Planner::collectPluginDependencies(const std::vector<Request>& requests) const {
	std::vector<Package> dependencies;

	for (const Request& request : requests) {
		if (this->gatewayExists(request.system)) {
			continue;
		}
		IPlugin* plugin = this->registry->getPlugin(request.system);
		if (plugin == nullptr) {
			continue;
		}

		for (Package dependency : plugin->getRequirements()) {
			dependencies.push_back(resolveDependencySystem(planner_normalize_dependency(std::move(dependency), request.system), this->registry));
		}
	}

	return dependencies;
}

std::vector<Package> Planner::filterMissingDependencies(const std::vector<Package>& dependencies) const {
	std::vector<Package> missingDependencies;
	std::map<std::string, std::vector<Package>> dependenciesBySystem;
	std::map<std::string, IPlugin*> pluginsBySystem;

	for (const Package& dependency : dependencies) {
		if (dependency.system.empty()) {
			missingDependencies.push_back(dependency);
			continue;
		}

		if (this->gatewayExists(dependency.system)) {
			missingDependencies.push_back(dependency);
			continue;
		}

		if (!this->dependencyExists(dependency) && this->config.planner.autoDownloadMissingDependencies) {
			this->queueDependencyDownload(dependency);
		}

		IPlugin* plugin = this->registry->getPlugin(dependency.system);
		if (plugin == nullptr || !this->registry->loadPlugin(dependency.system)) {
			missingDependencies.push_back(dependency);
			continue;
		}

		const std::string resolvedSystem = this->registry->resolvePluginName(dependency.system);
		dependenciesBySystem[resolvedSystem].push_back(dependency);
		pluginsBySystem[resolvedSystem] = plugin;
	}

	for (auto& [system, systemDependencies] : dependenciesBySystem) {
		IPlugin* plugin = pluginsBySystem[system];
		const std::vector<Package> pluginMissingDependencies = plugin->getMissingPackages(systemDependencies);
		missingDependencies.insert(missingDependencies.end(), pluginMissingDependencies.begin(), pluginMissingDependencies.end());
	}

	return missingDependencies;
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
	if (!dependency.system.empty() && this->gatewayExists(dependency.system)) {
		return true;
	}
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
	std::map<std::string, bool> includeDependenciesBySystem;

	for (const Request& request : requests) {
		const std::string resolvedSystem = this->registry->resolvePluginName(request.system);
		bool includeDependencies = false;
		if (request.action == ActionType::INSTALL && !this->gatewayExists(request.system)) {
			const auto it = includeDependenciesBySystem.find(resolvedSystem);
			if (it == includeDependenciesBySystem.end()) {
				includeDependencies = this->shouldInstallPluginDependencies(resolvedSystem);
				includeDependenciesBySystem.emplace(resolvedSystem, includeDependencies);
			} else {
				includeDependencies = it->second;
			}
		}

		this->addRequestToGraph(*graph, request, includeDependencies);
	}

	return graph;
}

Graph* Planner::buildDependencyDag(const std::vector<Package>& dependencies) const {
	Graph* graph = new Graph();

	for (const Package& dependency : dependencies) {
		this->addPackageToGraph(*graph, dependency);
	}

	return graph;
}

void Planner::addRequestToGraph(Graph& graph, const Request& request, bool includeDependencies) const {
	if (request.packages.empty()) {
		if (request.usesLocalTarget) {
			const Package package = this->makeLocalRequestedPackage(request);
			if (includeDependencies) {
				this->addPackageToGraph(graph, package);
				return;
			}
			(void)findOrAddPackageVertex(graph, package);
		}
		return;
	}

	for (const std::string& packageName : request.packages) {
		const Package package = this->makeRequestedPackage(request, packageName);
		if (includeDependencies) {
			this->addPackageToGraph(graph, package);
			continue;
		}

		(void)findOrAddPackageVertex(graph, package);
	}
}

void Planner::addPackageToGraph(Graph& graph, const Package& package) const {
	std::function<Graph::vertex_descriptor(const Package&, std::vector<std::string>&)> addPackageWithDependencies;
	addPackageWithDependencies = [&](const Package& currentPackage, std::vector<std::string>& activeSystems) -> Graph::vertex_descriptor {
		const Graph::vertex_descriptor packageVertex = findOrAddPackageVertex(graph, currentPackage);

		if (std::find(activeSystems.begin(), activeSystems.end(), currentPackage.system) != activeSystems.end()) {
			return packageVertex;
		}

		activeSystems.push_back(currentPackage.system);

		if (this->gatewayExists(currentPackage.system)) {
			activeSystems.pop_back();
			return packageVertex;
		}

		IPlugin* plugin = this->registry->getPlugin(currentPackage.system);
		if (plugin == nullptr && this->config.planner.autoDownloadMissingDependencies) {
			this->queueDependencyDownload(Package{.action = ActionType::INSTALL, .system = currentPackage.system});
			plugin = this->registry->getPlugin(currentPackage.system);
		}

		if (plugin != nullptr) {
			std::vector<Package> normalizedDependencies;
			for (Package dependency : plugin->getRequirements()) {
				normalizedDependencies.push_back(resolveDependencySystem(planner_normalize_dependency(std::move(dependency), currentPackage.system), this->registry));
			}

			for (const Package& missingDependency : this->filterMissingDependencies(normalizedDependencies)) {
				const Graph::vertex_descriptor dependencyVertex = addPackageWithDependencies(missingDependency, activeSystems);
				if (dependencyVertex != packageVertex && !boost::edge(dependencyVertex, packageVertex, graph).second) {
					boost::add_edge(dependencyVertex, packageVertex, graph);
				}
			}
		}

		activeSystems.pop_back();
		return packageVertex;
	};

	std::vector<std::string> activeSystems;
	(void)addPackageWithDependencies(package, activeSystems);
}

Package Planner::makeRequestedPackage(const Request& request, const std::string& packageSpecifier) const {
	return planner_make_requested_package(request, this->registry->resolvePluginName(request.system), packageSpecifier);
}

Package Planner::makeLocalRequestedPackage(const Request& request) const {
	return planner_make_local_requested_package(request, this->registry->resolvePluginName(request.system));
}

std::vector<Graph::vertex_descriptor> Planner::topologicallySort(const Graph& graph) const {
	std::vector<Graph::vertex_descriptor> order;
	boost::topological_sort(graph, std::back_inserter(order));
	std::reverse(order.begin(), order.end());
	return order;
}
