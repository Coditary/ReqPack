#include "core/validator.h"
#include <boost/graph/graph_traits.hpp>

#include <algorithm>
#include <cctype>
#include <iostream>

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

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
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

std::vector<Package> Validator::collectPackages(const Graph& graph) const {
	std::vector<Package> packages;
	auto [vertex, vertexEnd] = boost::vertices(graph);
	for (; vertex != vertexEnd; ++vertex) {
		packages.push_back(graph[*vertex]);
	}

	return packages;
}

std::vector<ValidationFinding> Validator::prepareSecurityData(const Graph& graph) const {
	if (!this->config.security.autoFetch) {
		return this->syncService.ensureReady();
	}

	if (this->config.security.osvRefreshMode == OsvRefreshMode::MANUAL && this->database.hasAdvisories()) {
		return this->syncService.ensureReady();
	}

	const std::set<std::string> ecosystems = this->securityGateway.resolvePackageEcosystems(this->collectPackages(graph));
	if (ecosystems.empty()) {
		return {};
	}

	return this->securityGateway.ensureEcosystemsReady(ecosystems);
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
	std::cerr << "unsafe findings require confirmation:\n";
	std::size_t shown = 0;
	for (const ValidationFinding& finding : findings) {
		std::string message = finding.message.empty() ? finding.id : finding.message;
		if (!finding.source.empty()) {
			message += " [" + finding.source + "]";
		}
		if (!finding.package.system.empty() && !finding.package.name.empty()) {
			message += " [" + finding.package.system + ":" + finding.package.name + "]";
		}
		std::cerr << "- " << message << '\n';
		++shown;
		if (shown >= 5) {
			break;
		}
	}

	if (!this->config.interaction.interactive) {
		std::cerr << "interactive mode is disabled; refusing to continue.\n";
		return false;
	}

	std::cerr << "Continue? [y/N] " << std::flush;
	std::string answer;
	if (!std::getline(std::cin, answer)) {
		std::cerr << "aborted.\n";
		return false;
	}

	const std::string normalizedAnswer = to_lower_copy(answer);
	if (normalizedAnswer == "y" || normalizedAnswer == "yes") {
		return true;
	}

	std::cerr << "aborted.\n";
	return false;
}

void Validator::generateReport(const Graph& graph, const std::vector<ValidationFinding>& findings) const {
	(void)graph;
	(void)findings;
	// Skeleton hook: later this can emit CycloneDX or similar reports.
}
