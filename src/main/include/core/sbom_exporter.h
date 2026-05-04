#pragma once

#include "core/configuration.h"
#include "core/plugin_metadata_provider.h"
#include "core/types.h"

#include <string>

class SbomExporter {
    ReqPackConfig config;
    PluginMetadataProvider* metadataProvider;

    SbomOutputFormat resolveFormat(const Request& request) const;
    std::string resolveOutputPath(const Request& request) const;
    std::string renderTable(const Graph& graph, bool colorizeTable, bool disableWrap, bool wideTable) const;
    std::string renderJson(const Graph& graph, bool cyclonedx) const;

public:
    explicit SbomExporter(PluginMetadataProvider* metadataProvider = nullptr, const ReqPackConfig& config = default_reqpack_config());

    std::string renderGraph(const Graph& graph, const Request& request) const;
    bool exportGraph(const Graph& graph, const Request& request) const;
};
