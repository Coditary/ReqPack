#pragma once

#include "core/export/sbom_exporter.h"

#include <string>
#include <vector>

namespace sbom_exporter_internal {

std::string to_lower_copy(std::string value);
std::vector<Graph::vertex_descriptor> ordered_vertices(const Graph& graph);
std::string sbom_component_ref(const Package& package);
std::string package_display_name(const Package& package);
bool table_colors_enabled();

}  // namespace sbom_exporter_internal
