#include "core/planning/planner.h"

#include "planner_internal.h"

#include "output/logger.h"

#include <optional>
#include <string>
#include <vector>

Planner::Planner(Registry* registry, RegistryDatabase* database, const ReqPackConfig& config)
    : config(config), downloader(database, config), registry(registry), securityGateway(registry, registry, config) {
    this->registry = registry;
}

Planner::~Planner() {}

Graph* Planner::plan(const std::vector<Request>& requests) {
    std::string resolutionError;
    const std::optional<std::vector<Request>> expandedRequests = this->resolveRequests(requests, &resolutionError);
    if (!expandedRequests.has_value()) {
        if (!resolutionError.empty()) {
            Logger::instance().err(resolutionError);
        }
        return nullptr;
    }
    this->ensurePluginsAvailable(expandedRequests.value());
    const std::vector<Request> filteredRequests = this->filterRequestedPackages(expandedRequests.value());

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
