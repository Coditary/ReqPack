#include "validator_internal.h"

#include "output/logger.h"

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

std::string to_lower_copy(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return value;
}

}  // namespace

namespace validator_internal {

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

}  // namespace validator_internal

bool Validator::requestUserDecision(const std::vector<ValidationFinding>& findings) const {
	Logger& logger = Logger::instance();
	logger.stdout("unsafe findings require confirmation:");
	std::size_t shown = 0;
	for (const ValidationFinding& finding : findings) {
		std::string message = finding.message.empty() ? finding.id : finding.message;
		if (!finding.source.empty()) {
			message += " [" + finding.source + "]";
		}
		if (!finding.package.system.empty() && !finding.package.name.empty()) {
			message += " [" + finding.package.system + ":" + finding.package.name + "]";
		}
		logger.stdout("- " + message);
		++shown;
		if (shown >= 5) {
			break;
		}
	}

	if (!this->config.interaction.interactive) {
		logger.diagnostic(make_error_diagnostic(
			"security",
			"interactive mode is disabled; refusing to continue.",
			"Security policy requires user confirmation, but interactive prompts are disabled.",
			"Re-run without `--non-interactive`, or switch `security.onUnsafe` to `abort` or `allow`."
		));
		logger.flushSync();
		return false;
	}

	logger.stdout("Continue? [y/N]");
	logger.flushSync();
	std::string answer;
	if (!std::getline(std::cin, answer)) {
		logger.stdout("aborted.");
		logger.flushSync();
		return false;
	}

	const std::string normalizedAnswer = to_lower_copy(answer);
	if (normalizedAnswer == "y" || normalizedAnswer == "yes") {
		return true;
	}

	logger.stdout("aborted.");
	logger.flushSync();
	return false;
}
