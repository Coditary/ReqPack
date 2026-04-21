#include "core/validator.h"

#include <boost/graph/graph_traits.hpp>

#include <algorithm>

Validator::Validator() {}

Validator::~Validator() {}

Graph* Validator::validate(Graph *graph) {
	if (graph == nullptr) {
		return nullptr;
	}

	const ValidationPolicy policy = this->loadPolicy();
	const std::vector<ValidationFinding> findings = this->scanGraph(*graph);

	if (policy.generateReport) {
		this->generateReport(*graph, findings);
	}

	if (this->exceedsThreshold(findings, policy)) {
		if (!policy.promptOnUnsafe) {
			return nullptr;
		}

		if (!this->requestUserDecision(findings)) {
			return nullptr;
		}
	}

	if (this->shouldPromptUser(findings, policy) && !this->requestUserDecision(findings)) {
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

std::vector<Validator::ValidationFinding> Validator::scanGraph(const Graph& graph) const {
	std::vector<ValidationFinding> findings;

	for (const Package& package : this->collectPackages(graph)) {
		const std::vector<ValidationFinding> packageFindings = this->scanPackage(package);
		findings.insert(findings.end(), packageFindings.begin(), packageFindings.end());
	}

	return findings;
}

std::vector<Validator::ValidationFinding> Validator::scanPackage(const Package& package) const {
	std::vector<ValidationFinding> findings = this->runSnykScan(package);
	const std::vector<ValidationFinding> owaspFindings = this->runOwaspScan(package);
	findings.insert(findings.end(), owaspFindings.begin(), owaspFindings.end());
	return findings;
}

std::vector<Validator::ValidationFinding> Validator::runSnykScan(const Package& package) const {
	(void)package;
	// Skeleton hook: later this will run a Snyk-backed scan and translate results into findings.
	return {};
}

std::vector<Validator::ValidationFinding> Validator::runOwaspScan(const Package& package) const {
	(void)package;
	// Skeleton hook: later this will run OWASP or similar scanners and translate results into findings.
	return {};
}

Validator::ValidationPolicy Validator::loadPolicy() const {
	ValidationPolicy policy;
	policy.promptOnUnsafe = DEFAULT_REQPACK_CONFIG.security.promptOnUnsafe ||
		DEFAULT_REQPACK_CONFIG.security.onUnsafe == UnsafeAction::PROMPT;
	policy.abortThreshold = to_string(DEFAULT_REQPACK_CONFIG.security.severityThreshold);
	policy.abortScoreThreshold = DEFAULT_REQPACK_CONFIG.security.scoreThreshold;
	policy.generateReport = DEFAULT_REQPACK_CONFIG.reports.enabled;
	return policy;
}

bool Validator::exceedsThreshold(const std::vector<ValidationFinding>& findings, const ValidationPolicy& policy) const {
	const int thresholdRank = this->severityRank(policy.abortThreshold);
	for (const ValidationFinding& finding : findings) {
		if (this->severityRank(finding.severity) >= thresholdRank) {
			return true;
		}

		if (policy.abortScoreThreshold > 0.0 && finding.score >= policy.abortScoreThreshold) {
			return true;
		}
	}

	return false;
}

bool Validator::shouldPromptUser(const std::vector<ValidationFinding>& findings, const ValidationPolicy& policy) const {
	return policy.promptOnUnsafe && !findings.empty();
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

int Validator::severityRank(const std::string& severity) const {
	if (severity == "critical") {
		return 4;
	}
	if (severity == "high") {
		return 3;
	}
	if (severity == "medium") {
		return 2;
	}
	if (severity == "low") {
		return 1;
	}
	if (severity == "unassigned") {
		return 0;
	}

	return 0;
}
