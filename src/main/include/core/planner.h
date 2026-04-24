#pragma once

#include "core/configuration.h"
#include "core/downloader.h"
#include "core/types.h"
#include "core/registry.h"

class Planner {
	ReqPackConfig config;
	Downloader downloader;
	Registry* registry;

	std::vector<Request> expandProxies(const std::vector<Request>& requests) const;
	void ensurePluginsAvailable(const std::vector<Request>& requests) const;
	bool pluginExists(const std::string& system) const;
	void queuePluginDownload(const std::string& system) const;
	bool shouldInstallPluginDependencies(const std::string& system) const;
	bool pluginRequirementsSatisfied(const std::string& system) const;
	bool pluginRequirementsProvisioned(const std::string& system) const;
	void markPluginRequirementsProvisioned(const std::string& system) const;

	std::vector<Request> filterRequestedPackages(const std::vector<Request>& requests) const;
	std::vector<Package> collectPluginDependencies(const std::vector<Request>& requests) const;
	std::vector<Package> filterMissingDependencies(const std::vector<Package>& dependencies) const;
	void ensurePluginDependenciesAvailable(const std::vector<Package>& dependencies) const;
	bool dependencyExists(const Package& dependency) const;
	void queueDependencyDownload(const Package& dependency) const;

	Graph* buildDag(const std::vector<Request>& requests) const;
	Graph* buildDependencyDag(const std::vector<Package>& dependencies) const;
	void addRequestToGraph(Graph& graph, const Request& request, bool includeDependencies) const;
	void addPackageToGraph(Graph& graph, const Package& package) const;
	Package makeRequestedPackage(const Request& request, const std::string& packageSpecifier) const;
	Package makeLocalRequestedPackage(const Request& request) const;
	std::vector<Graph::vertex_descriptor> topologicallySort(const Graph& graph) const;

public:
	Planner(Registry* registry, RegistryDatabase* database, const ReqPackConfig& config = DEFAULT_REQPACK_CONFIG);
	~Planner();

	Graph* plan(const std::vector<Request>& requests);
};
