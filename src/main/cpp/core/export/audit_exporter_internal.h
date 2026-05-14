#pragma once

#include "core/export/audit_exporter.h"

#include <string>

namespace audit_exporter_internal {

std::string to_lower_copy(std::string value);
std::string package_component_ref(const Package& package);
std::string package_display_name(const Package& package);
bool table_colors_enabled();

}  // namespace audit_exporter_internal
