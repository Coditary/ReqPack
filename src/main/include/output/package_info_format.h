#pragma once

#include "core/common/types.h"

#include <string>

/// Renders a structured, hierarchical package info view for `rqp info`.
std::string format_package_info(const PackageInfo& info);
