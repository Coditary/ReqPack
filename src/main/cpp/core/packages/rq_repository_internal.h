#pragma once

#include "core/packages/rq_repository.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace rq_repository_internal {

RqRepositoryIndex parse_index_impl(const std::string& content, const std::string& source);

std::optional<RqRepositoryPackage> resolve_package_impl(
    const std::vector<RqRepositoryIndex>& indexes,
    const std::string& name,
    const std::string& version,
    const std::string& hostArchitecture,
    const std::set<std::string>& hostSystems,
    const ReqPackConfig& config
);

}  // namespace rq_repository_internal
