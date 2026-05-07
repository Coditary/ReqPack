#pragma once

#include "core/config/configuration.h"
#include "core/download/downloader.h"
#include "core/planning/planner_core.h"
#include "core/security/security_gateway_service.h"
#include "core/common/types.h"
#include "core/registry/registry.h"

class Planner {
	ReqPackConfig config;
	Downloader downloader;
	Registry* registry;
	SecurityGatewayService securityGateway;

	std::vector<Request> expandProxies(const std::vector<Request>& requests) const;
	std::optional<std::vector<Request>> resolveRequests(const std::vector<Request>& requests, std::string* errorMessage = nullptr) const;
	void ensurePluginsAvailable(const std::vector<Request>& requests) const;
	bool pluginExists(const std::string& system) const;
	bool gatewayExists(const std::string& system) const;
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
	void addRequestToGraph(Graph& graph, const Request& request, bool includeDependencies, bool directRequest = true) const;
	void addPackageToGraph(Graph& graph, const Package& package) const;
	Package makeRequestedPackage(const Request& request, const std::string& packageSpecifier) const;
	Package makeLocalRequestedPackage(const Request& request) const;
	std::vector<Graph::vertex_descriptor> topologicallySort(const Graph& graph) const;

public:
	Planner(Registry* registry, RegistryDatabase* database, const ReqPackConfig& config = default_reqpack_config());
	~Planner();

	Graph* plan(const std::vector<Request>& requests);
};
