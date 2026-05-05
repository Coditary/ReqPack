#pragma once

#include "core/configuration.h"
#include "core/audit_exporter.h"
#include "core/registry_database.h"
#include "core/registry.h"
#include "core/planner.h"
#include "core/sbom_exporter.h"
#include "core/snapshot_exporter.h"
#include "core/validator.h"
#include "core/executor.h"

#include "core/types.h"


class Orchestrator {

	Registry*  registry;
	Planner*   planner;
	AuditExporter* auditExporter;
	SbomExporter* sbomExporter;
	SnapshotExporter* snapshotExporter;
	Validator* validator;
	Executer*  executor;
	ReqPackConfig config;
	std::vector<Request> requests;


public:
	Orchestrator(std::vector<Request> requests, const ReqPackConfig& config = default_reqpack_config());
	~Orchestrator();

	int countRequestedItems() const;
	int run();

private:
	bool shouldRefreshPluginWrappers() const;
	bool shouldRunSystemWidePackageUpdates() const;
	int runPluginWrapperRefresh();
	int runSystemWidePackageUpdates();
};
