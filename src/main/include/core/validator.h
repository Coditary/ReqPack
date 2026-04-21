#pragma once

#include "core/configuration.h"
#include "core/types.h"

#include <string>
#include <vector>

class Validator {
	ReqPackConfig config;

	struct ValidationFinding {
		Package package;
		std::string source;
		std::string severity;
		double score{0.0};
		std::string message;
	};

	struct ValidationPolicy {
		bool promptOnUnsafe{false};
		std::string abortThreshold{"critical"};
		double abortScoreThreshold{0.0};
		bool generateReport{false};
	};

	std::vector<Package> collectPackages(const Graph& graph) const;
	std::vector<ValidationFinding> scanGraph(const Graph& graph) const;
	std::vector<ValidationFinding> scanPackage(const Package& package) const;
	std::vector<ValidationFinding> runSnykScan(const Package& package) const;
	std::vector<ValidationFinding> runOwaspScan(const Package& package) const;
	ValidationPolicy loadPolicy() const;
	bool exceedsThreshold(const std::vector<ValidationFinding>& findings, const ValidationPolicy& policy) const;
	bool shouldPromptUser(const std::vector<ValidationFinding>& findings, const ValidationPolicy& policy) const;
	bool requestUserDecision(const std::vector<ValidationFinding>& findings) const;
	void generateReport(const Graph& graph, const std::vector<ValidationFinding>& findings) const;
	int severityRank(const std::string& severity) const;

public:
	Validator(const ReqPackConfig& config = DEFAULT_REQPACK_CONFIG);
	~Validator();

	Graph* validate(Graph *graph);
};
