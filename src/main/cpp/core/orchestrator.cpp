#include "core/orchestrator.h"

Orchestrator::Orchestrator() {
	this->registry  = new Registry();
	this->planner   = new Planner(this->registry);
	this->validator = new Validator();
	this->executor  = new Executer();
}

Orchestrator::~Orchestrator() {
	delete this->registry;
	delete this->planner;
	delete this->validator;
	delete this->executor;
}

void Orchestrator::run() {
	Graph* graph;
	this->registry->scanDirectory("./plugins");

	graph = this->planner->plan();
	graph = this->validator->validate(graph);
	this->executor->execute(graph);
}
