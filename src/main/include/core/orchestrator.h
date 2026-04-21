#pragma once

#include "core/configuration.h"
#include "core/registry_database.h"
#include "core/registry.h"
#include "core/planner.h"
#include "core/validator.h"
#include "core/executor.h"

#include "core/types.h"


class Orchestrator {

	Registry*  registry;
	Planner*   planner;
	Validator* validator;
	Executer*  executor;
	ReqPackConfig config;
	std::vector<Request> requests;


public:
	Orchestrator(std::vector<Request> requests, const ReqPackConfig& config = DEFAULT_REQPACK_CONFIG);
	~Orchestrator();

	void run();
};
