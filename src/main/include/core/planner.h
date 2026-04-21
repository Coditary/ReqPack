#pragma once

#include "core/types.h"
#include "core/registry.h"

class Planner {
	Registry* registry;

	std::vector<Request> expandProxies(const std::vector<Request>& requests) const;
	void ensurePluginsAvailable(const std::vector<Request>& requests) const;
	bool pluginExists(const std::string& system) const;
	void queuePluginDownload(const std::string& system) const;

	std::vector<Package> collectPluginDependencies(const std::vector<Request>& requests) const;
	void ensurePluginDependenciesAvailable(const std::vector<Package>& dependencies) const;
	bool dependencyExists(const Package& dependency) const;
	void queueDependencyDownload(const Package& dependency) const;

	Graph* buildDag(const std::vector<Request>& requests) const;
	void addRequestToGraph(Graph& graph, const Request& request) const;
	std::vector<Graph::vertex_descriptor> topologicallySort(const Graph& graph) const;

public:
	Planner(Registry* registry);
	~Planner();

	Graph* plan(const std::vector<Request>& requests);
};
