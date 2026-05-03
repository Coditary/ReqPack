#pragma once

#include "core/configuration.h"
#include "core/plugin_metadata_provider.h"
#include "core/types.h"
#include "core/validator_core.h"

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
    explicit AuditExporter(PluginMetadataProvider* metadataProvider = nullptr, const ReqPackConfig& config = DEFAULT_REQPACK_CONFIG);

    std::string renderGraph(const Graph& graph, const std::vector<ValidationFinding>& findings, const Request& request) const;
    bool exportGraph(const Graph& graph, const std::vector<ValidationFinding>& findings, const Request& request) const;
};
