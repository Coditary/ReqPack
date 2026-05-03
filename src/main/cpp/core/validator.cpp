#include "core/validator.h"
#include <boost/graph/graph_traits.hpp>

#include <algorithm>

namespace {

bool graph_contains_only_remove_actions(const Graph& graph) {
    auto [vertex, vertexEnd] = boost::vertices(graph);
    if (vertex == vertexEnd) {
        return false;
    }

    for (; vertex != vertexEnd; ++vertex) {
        if (graph[*vertex].action != ActionType::REMOVE) {
            return false;
        }
    }

    return true;
}

std::vector<ValidationFinding> disposition_findings(const Graph& graph, const std::vector<ValidationFinding>& findings) {
    std::vector<ValidationFinding> filtered;
    filtered.reserve(findings.size());
    const bool removeOnlyGraph = graph_contains_only_remove_actions(graph);
    for (const ValidationFinding& finding : findings) {
        if (finding.kind == "vulnerability" && (removeOnlyGraph || finding.package.action == ActionType::REMOVE)) {
            continue;
        }
        filtered.push_back(finding);
    }
    return filtered;
}

}  // namespace

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
	std::vector<ValidationFinding> findings;
	if (this->config.security.autoFetch && !this->config.security.gateways.empty() &&
	    !(this->config.security.osvRefreshMode == OsvRefreshMode::MANUAL && this->database.hasAdvisories())) {
		findings = this->securityGateway.ensureEcosystemsReady(this->securityGateway.resolvePackageEcosystems(this->collectPackages(*graph)));
	} else {
		findings = this->syncService.ensureReady();
	}
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

	const std::vector<ValidationFinding> dispositionFindings = disposition_findings(*graph, findings);
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

	std::vector<ValidationFinding> findings;
	if (this->config.security.autoFetch && !this->config.security.gateways.empty() &&
	    !(this->config.security.osvRefreshMode == OsvRefreshMode::MANUAL && this->database.hasAdvisories())) {
		findings = this->securityGateway.ensureEcosystemsReady(this->securityGateway.resolvePackageEcosystems(this->collectPackages(*graph)));
	} else {
		findings = this->syncService.ensureReady();
	}
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

std::vector<Package> Validator::collectPackages(const Graph& graph) const {
	std::vector<Package> packages;
	auto [vertex, vertexEnd] = boost::vertices(graph);
	for (; vertex != vertexEnd; ++vertex) {
		packages.push_back(graph[*vertex]);
	}

	return packages;
}

std::vector<ValidationFinding> Validator::scanGraph(const Graph& graph) const {
	VulnerabilityMatcher matcher = this->matcher;
	matcher.setDatabase(&this->database);
	matcher.setAdvisories(this->loadAdvisories());
	return matcher.matchGraph(graph);
}

std::vector<OsvAdvisory> Validator::loadAdvisories() const {
	std::vector<OsvAdvisory> advisories;
	if (!this->config.security.osvOverlayPath.empty()) {
		const std::vector<OsvAdvisory> overlay = osv_load_advisories_from_path(this->config.security.osvOverlayPath);
		advisories.insert(advisories.end(), overlay.begin(), overlay.end());
	}

	return advisories;
}

bool Validator::requestUserDecision(const std::vector<ValidationFinding>& findings) const {
	(void)findings;
	// Skeleton hook: later this will ask the user whether execution should continue.
	return true;
}

void Validator::generateReport(const Graph& graph, const std::vector<ValidationFinding>& findings) const {
	(void)graph;
	(void)findings;
	// Skeleton hook: later this can emit CycloneDX or similar reports.
}
