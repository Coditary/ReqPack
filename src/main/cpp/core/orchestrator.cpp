#include "core/orchestrator.h"

#include <utility>

Orchestrator::Orchestrator(std::vector<Request> requests) : requests(std::move(requests)) {
	this->registry  = new Registry();
	this->planner   = new Planner(this->registry);
	this->validator = new Validator();
	this->executor  = new Executer(this->registry);
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

	graph = this->planner->plan(this->requests);
	graph = this->validator->validate(graph);
	this->executor->execute(graph);
}
