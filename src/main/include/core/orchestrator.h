#pragma once

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
	std::vector<Request> requests;


public:
	Orchestrator(std::vector<Request> requests);
	~Orchestrator();

	void run();
};
