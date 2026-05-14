#include "core/security/validator.h"

#include "validator_internal.h"

Validator::Validator(PluginMetadataProvider* metadataProvider, const ReqPackConfig& config)
	: config(config),
	  metadataProvider(metadataProvider),
	  database(config),
	  syncService(&this->database, metadataProvider, config),
	  securityGateway(nullptr, metadataProvider, config),
	  matcher(metadataProvider, config) {}

Validator::~Validator() {}

Graph* Validator::validate(Graph *graph) {
	this->lastFindings.clear();
	if (graph == nullptr) {
		return nullptr;
	}

	const ValidationPolicy policy = validator_policy_from_config(this->config);
	std::vector<ValidationFinding> findings = this->prepareSecurityData(*graph);
	const std::vector<ValidationFinding> graphFindings = this->scanGraph(*graph);
	findings.insert(findings.end(), graphFindings.begin(), graphFindings.end());
	findings = validator_apply_rules(
		findings,
		this->config.security.allowVulnerabilityIds,
		this->config.security.ignoreVulnerabilityIds
	);
	this->lastFindings = findings;

	if (policy.generateReport) {
		this->generateReport(*graph, findings);
	}

	const std::vector<ValidationFinding> dispositionFindings = validator_internal::disposition_findings(*graph, findings);
	const ValidationDisposition disposition = validator_disposition(dispositionFindings, policy);
	if (disposition == ValidationDisposition::Abort) {
		return nullptr;
	}

	if (disposition == ValidationDisposition::Prompt && !this->requestUserDecision(dispositionFindings)) {
			return nullptr;
	}

	return graph;
}

std::vector<ValidationFinding> Validator::audit(Graph* graph) {
	this->lastFindings.clear();
	if (graph == nullptr) {
		return {};
	}

	std::vector<ValidationFinding> findings = this->prepareSecurityData(*graph);
	const std::vector<ValidationFinding> graphFindings = this->scanGraph(*graph);
	findings.insert(findings.end(), graphFindings.begin(), graphFindings.end());
	findings = validator_apply_rules(
		findings,
		this->config.security.allowVulnerabilityIds,
		this->config.security.ignoreVulnerabilityIds
	);
	this->lastFindings = findings;
	return findings;
}

const std::vector<ValidationFinding>& Validator::getLastFindings() const {
	return this->lastFindings;
}

void Validator::generateReport(const Graph& graph, const std::vector<ValidationFinding>& findings) const {
	(void)graph;
	(void)findings;
	// Skeleton hook: later this can emit CycloneDX or similar reports.
}
