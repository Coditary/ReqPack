#pragma once

#include "core/config/configuration.h"
#include "core/plugins/plugin_metadata_provider.h"
#include "core/common/types.h"
#include "core/security/validator_core.h"

#include <string>
#include <vector>

class AuditExporter {
    ReqPackConfig config;
    PluginMetadataProvider* metadataProvider;

    AuditOutputFormat resolveFormat(const Request& request) const;
    std::string resolveOutputPath(const Request& request) const;
    std::string renderTable(
        const Graph& graph,
        const std::vector<ValidationFinding>& findings,
        bool colorizeSeverity,
        bool disableWrap,
        bool wideTable
    ) const;
    std::string renderJson(const Graph& graph, const std::vector<ValidationFinding>& findings) const;
    std::string renderCycloneDxVex(const Graph& graph, const std::vector<ValidationFinding>& findings) const;
    std::string renderSarif(const Graph& graph, const std::vector<ValidationFinding>& findings) const;

public:
    explicit AuditExporter(PluginMetadataProvider* metadataProvider = nullptr, const ReqPackConfig& config = default_reqpack_config());

    std::string renderGraph(const Graph& graph, const std::vector<ValidationFinding>& findings, const Request& request) const;
    bool exportGraph(const Graph& graph, const std::vector<ValidationFinding>& findings, const Request& request) const;
};
