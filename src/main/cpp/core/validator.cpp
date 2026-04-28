#include "core/validator.h"

#include <boost/graph/graph_traits.hpp>

#include <algorithm>

Validator::Validator(PluginMetadataProvider* metadataProvider, const ReqPackConfig& config)
	: config(config), metadataProvider(metadataProvider), database(config), syncService(&this->database, metadataProvider, config), matcher(metadataProvider, config) {}

Validator::~Validator() {}

Graph* Validator::validate(Graph *graph) {
	if (graph == nullptr) {
		return nullptr;
	}

	const ValidationPolicy policy = validator_policy_from_config(this->config);
	std::vector<ValidationFinding> findings = this->syncService.ensureReady();
	const std::vector<ValidationFinding> graphFindings = this->scanGraph(*graph);
	findings.insert(findings.end(), graphFindings.begin(), graphFindings.end());
	findings = validator_apply_rules(
		findings,
		this->config.security.allowVulnerabilityIds,
		this->config.security.ignoreVulnerabilityIds
	);

	if (policy.generateReport) {
		this->generateReport(*graph, findings);
	}

	const ValidationDisposition disposition = validator_disposition(findings, policy);
	if (disposition == ValidationDisposition::Abort) {
		return nullptr;
	}

	if (disposition == ValidationDisposition::Prompt && !this->requestUserDecision(findings)) {
			return nullptr;
	}

	return graph;
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
