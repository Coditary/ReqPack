#include "rq_repository_internal.h"

#include "core/common/version_compare.h"
#include "core/packages/rq_package.h"

namespace {

bool is_better_candidate(const RqRepositoryPackage& candidate, const RqRepositoryPackage& currentBest) {
    const int versionComparison = version_compare_values(candidate.version, currentBest.version);
    if (versionComparison != 0) {
        return versionComparison > 0;
    }
    if (candidate.release != currentBest.release) {
        return candidate.release > currentBest.release;
    }
    if (candidate.revision != currentBest.revision) {
        return candidate.revision > currentBest.revision;
    }
    return false;
}

}  // namespace

namespace rq_repository_internal {

std::optional<RqRepositoryPackage> resolve_package_impl(
    const std::vector<RqRepositoryIndex>& indexes,
    const std::string& name,
    const std::string& version,
    const std::string& hostArchitecture,
    const std::set<std::string>& hostSystems,
    const ReqPackConfig& config
) {
    std::optional<RqRepositoryPackage> best;
    const auto aliases = rq_merged_system_aliases(config);

    for (const RqRepositoryIndex& index : indexes) {
        for (const RqRepositoryPackage& package : index.packages) {
            if (package.name != name) {
                continue;
            }
            if (!version.empty() && package.version != version) {
                continue;
            }
            if (!rq_architecture_matches(package.architecture, hostArchitecture)) {
                continue;
            }
            if (!rq_system_matches(package.systems, hostSystems, aliases)) {
                continue;
            }
            if (!best.has_value() || is_better_candidate(package, best.value())) {
                best = package;
            }
        }
    }

    return best;
}

}  // namespace rq_repository_internal
