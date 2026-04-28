#pragma once

#include "core/configuration.h"
#include "core/osv_core.h"
#include "core/plugin_metadata_provider.h"
#include "core/types.h"
#include "core/validator_core.h"
#include "core/vulnerability_database.h"
#include "core/vulnerability_matcher.h"
#include "core/vulnerability_sync_service.h"

#include <string>
#include <vector>

class Validator {
	ReqPackConfig config;
	PluginMetadataProvider* metadataProvider;
	VulnerabilityDatabase database;
	VulnerabilitySyncService syncService;
	VulnerabilityMatcher matcher;

	std::vector<Package> collectPackages(const Graph& graph) const;
	std::vector<OsvAdvisory> loadAdvisories() const;

protected:
	virtual std::vector<ValidationFinding> scanGraph(const Graph& graph) const;
	virtual bool requestUserDecision(const std::vector<ValidationFinding>& findings) const;
	virtual void generateReport(const Graph& graph, const std::vector<ValidationFinding>& findings) const;

public:
	Validator(PluginMetadataProvider* metadataProvider = nullptr, const ReqPackConfig& config = DEFAULT_REQPACK_CONFIG);
	virtual ~Validator();

	Graph* validate(Graph *graph);
};
