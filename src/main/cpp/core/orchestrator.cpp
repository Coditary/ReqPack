#include "core/orchestrator.h"

#include <utility>

Orchestrator::Orchestrator(std::vector<Request> requests, const ReqPackConfig& config) : config(config), requests(std::move(requests)) {
	this->registry  = new Registry(this->config);
	this->planner   = new Planner(this->registry, this->config);
	this->validator = new Validator(this->config);
	this->executor  = new Executer(this->registry, this->config);
}

Orchestrator::~Orchestrator() {
	delete this->registry;
	delete this->planner;
	delete this->validator;
	delete this->executor;
}

void Orchestrator::run() {
	Graph* graph;
	this->registry->scanDirectory(this->config.registry.pluginDirectory);

	graph = this->planner->plan(this->requests);
	graph = this->validator->validate(graph);
	this->executor->execute(graph);
}
