#include "validator_internal.h"

#include <boost/graph/graph_traits.hpp>

#include <set>

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
