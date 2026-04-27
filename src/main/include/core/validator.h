#pragma once

#include "core/configuration.h"
#include "core/types.h"
#include "core/validator_core.h"

#include <string>
#include <vector>

class Validator {
	ReqPackConfig config;

	std::vector<Package> collectPackages(const Graph& graph) const;
	std::vector<ValidationFinding> scanPackage(const Package& package) const;
	std::vector<ValidationFinding> runSnykScan(const Package& package) const;
	std::vector<ValidationFinding> runOwaspScan(const Package& package) const;

protected:
	virtual std::vector<ValidationFinding> scanGraph(const Graph& graph) const;
	virtual bool requestUserDecision(const std::vector<ValidationFinding>& findings) const;
	virtual void generateReport(const Graph& graph, const std::vector<ValidationFinding>& findings) const;

public:
	Validator(const ReqPackConfig& config = DEFAULT_REQPACK_CONFIG);
	virtual ~Validator();

	Graph* validate(Graph *graph);
};
