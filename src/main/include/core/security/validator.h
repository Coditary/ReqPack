#pragma once

#include "core/config/configuration.h"
#include "core/security/osv_core.h"
#include "core/plugins/plugin_metadata_provider.h"
#include "core/security/security_gateway_service.h"
#include "core/common/types.h"
#include "core/security/validator_core.h"
#include "core/security/vulnerability_database.h"
#include "core/security/vulnerability_matcher.h"
#include "core/security/vulnerability_sync_service.h"

#include <string>
#include <vector>

class Validator {
	ReqPackConfig config;
	PluginMetadataProvider* metadataProvider;
	VulnerabilityDatabase database;
	VulnerabilitySyncService syncService;
	SecurityGatewayService securityGateway;
	VulnerabilityMatcher matcher;
	std::vector<ValidationFinding> lastFindings;

	std::vector<Package> collectPackages(const Graph& graph) const;
	std::vector<ValidationFinding> prepareSecurityData(const Graph& graph) const;
	std::vector<OsvAdvisory> loadAdvisories() const;

protected:
	virtual std::vector<ValidationFinding> scanGraph(const Graph& graph) const;
	virtual bool requestUserDecision(const std::vector<ValidationFinding>& findings) const;
	virtual void generateReport(const Graph& graph, const std::vector<ValidationFinding>& findings) const;

public:
	Validator(PluginMetadataProvider* metadataProvider = nullptr, const ReqPackConfig& config = default_reqpack_config());
	virtual ~Validator();

	Graph* validate(Graph *graph);
	std::vector<ValidationFinding> audit(Graph* graph);
	const std::vector<ValidationFinding>& getLastFindings() const;
};
