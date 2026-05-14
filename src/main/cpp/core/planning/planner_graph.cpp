#include "planner_internal.h"

#include "core/plugins/plugin_bundle.h"

#include <algorithm>
#include <boost/graph/topological_sort.hpp>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace planner_internal {

Graph::vertex_descriptor find_or_add_package_vertex(Graph& graph, const Package& package) {
    auto [vertex, vertexEnd] = boost::vertices(graph);
    for (; vertex != vertexEnd; ++vertex) {
        if (planner_same_package(graph[*vertex], package)) {
            graph[*vertex].directRequest = graph[*vertex].directRequest || package.directRequest;
            return *vertex;
        }
    }

    return boost::add_vertex(package, graph);
}

}  // namespace planner_internal

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

void Planner::addRequestToGraph(Graph& graph, const Request& request, bool includeDependencies, bool directRequest) const {
    if (request.packages.empty()) {
        if (request.usesLocalTarget) {
            Package package = this->makeLocalRequestedPackage(request);
            package.directRequest = directRequest;
            if (includeDependencies) {
                this->addPackageToGraph(graph, package);
                return;
            }
            (void)planner_internal::find_or_add_package_vertex(graph, package);
        }
        return;
    }

    for (const std::string& packageName : request.packages) {
        Package package = this->makeRequestedPackage(request, packageName);
        package.directRequest = directRequest;
        if (includeDependencies) {
            this->addPackageToGraph(graph, package);
            continue;
        }

        (void)planner_internal::find_or_add_package_vertex(graph, package);
    }
}

void Planner::addPackageToGraph(Graph& graph, const Package& package) const {
    std::function<Graph::vertex_descriptor(const Package&, std::vector<std::string>&)> addPackageWithDependencies;
    addPackageWithDependencies = [&](const Package& currentPackage, std::vector<std::string>& activeSystems) -> Graph::vertex_descriptor {
        const Graph::vertex_descriptor packageVertex = planner_internal::find_or_add_package_vertex(graph, currentPackage);

        if (std::find(activeSystems.begin(), activeSystems.end(), currentPackage.system) != activeSystems.end()) {
            return packageVertex;
        }

        activeSystems.push_back(currentPackage.system);

        if (this->gatewayExists(currentPackage.system)) {
            activeSystems.pop_back();
            return packageVertex;
        }

        std::optional<PluginBundleLayout> layout = plugin_bundle_find_installed(this->config, currentPackage.system);
        if (!layout.has_value() && this->config.planner.autoDownloadMissingDependencies) {
            this->queueDependencyDownload(Package{.action = ActionType::INSTALL, .system = currentPackage.system});
            layout = plugin_bundle_find_installed(this->config, currentPackage.system);
        }

        if (layout.has_value()) {
            std::vector<Package> normalizedDependencies;
            for (Package dependency : plugin_bundle_dependency_packages(layout.value())) {
                Package normalizedDependency = planner_internal::resolve_dependency_system(
                    planner_normalize_dependency(std::move(dependency), currentPackage.system),
                    this->registry
                );
                if (currentPackage.action == ActionType::ENSURE) {
                    normalizedDependency.action = ActionType::ENSURE;
                    planner_internal::propagate_internal_ensure_order_flag(currentPackage, normalizedDependency);
                }
                normalizedDependency.directRequest = false;
                normalizedDependencies.push_back(std::move(normalizedDependency));
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
