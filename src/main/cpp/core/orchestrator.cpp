#include "core/orchestrator.h"

Orchestrator::Orchestrator() {
	registry  = new Registry();
	planner   = new Planner();
	validator = new Validator();
	executor  = new Executer();
}

Orchestrator::~Orchestrator() {
	delete registry;
	delete planner;
	delete validator;
	delete executor;
}

void Orchestrator::run() {
	Graph* graph;
	registry->load();
	graph = planner->plan();
	graph = validator->validate(graph);
	executor->execute(graph);
}
