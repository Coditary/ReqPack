#include "core/validator.h"

#include <boost/graph/graph_traits.hpp>

#include <algorithm>

Validator::Validator(const ReqPackConfig& config) : config(config) {}

Validator::~Validator() {}

Graph* Validator::validate(Graph *graph) {
	if (graph == nullptr) {
		return nullptr;
	}

	const ValidationPolicy policy = validator_policy_from_config(this->config);
	const std::vector<ValidationFinding> findings = this->scanGraph(*graph);

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
	std::vector<ValidationFinding> findings;

	for (const Package& package : this->collectPackages(graph)) {
		const std::vector<ValidationFinding> packageFindings = this->scanPackage(package);
		findings.insert(findings.end(), packageFindings.begin(), packageFindings.end());
	}

	return findings;
}

std::vector<ValidationFinding> Validator::scanPackage(const Package& package) const {
	std::vector<ValidationFinding> findings = this->runSnykScan(package);
	const std::vector<ValidationFinding> owaspFindings = this->runOwaspScan(package);
	findings.insert(findings.end(), owaspFindings.begin(), owaspFindings.end());
	return findings;
}

std::vector<ValidationFinding> Validator::runSnykScan(const Package& package) const {
	(void)package;
	// Skeleton hook: later this will run a Snyk-backed scan and translate results into findings.
	return {};
}

std::vector<ValidationFinding> Validator::runOwaspScan(const Package& package) const {
	(void)package;
	// Skeleton hook: later this will run OWASP or similar scanners and translate results into findings.
	return {};
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
