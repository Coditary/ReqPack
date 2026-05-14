#pragma once

#include "core/planning/planner.h"

#include "plugins/iplugin.h"

#include <filesystem>

namespace planner_internal {

constexpr const char* INTERNAL_ENSURE_ORDER_FLAG_PREFIX = "__reqpack-internal-ensure-order=";

std::filesystem::path requirements_marker_path(const ReqPackConfig& config, const std::string& system);
void set_internal_ensure_order_flag(Package& package, std::size_t order);
void propagate_internal_ensure_order_flag(const Package& source, Package& target);
Package resolve_dependency_system(Package dependency, const Registry* registry);
IPlugin* load_plugin_for_use(Registry* registry, const std::string& system);
Graph::vertex_descriptor find_or_add_package_vertex(Graph& graph, const Package& package);

}  // namespace planner_internal
