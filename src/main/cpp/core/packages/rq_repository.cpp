#include "core/packages/rq_repository.h"

#include "rq_repository_internal.h"

RqRepositoryIndex rq_repository_parse_index(const std::string& content, const std::string& source) {
    return rq_repository_internal::parse_index_impl(content, source);
}

std::optional<RqRepositoryPackage> rq_repository_resolve_package(
    const std::vector<RqRepositoryIndex>& indexes,
    const std::string& name,
    const std::string& version,
    const std::string& hostArchitecture,
    const std::set<std::string>& hostSystems,
    const ReqPackConfig& config
) {
    return rq_repository_internal::resolve_package_impl(indexes, name, version, hostArchitecture, hostSystems, config);
}
