#pragma once

#include "core/config/configuration.h"
#include "core/export/audit_exporter.h"
#include "core/registry/registry_database.h"
#include "core/registry/registry.h"
#include "core/planning/planner.h"
#include "core/export/sbom_exporter.h"
#include "core/export/snapshot_exporter.h"
#include "core/security/validator.h"
#include "core/execution/executor.h"

#include "core/common/types.h"


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
