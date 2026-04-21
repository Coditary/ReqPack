#include "core/planner.h"

Planner::Planner(Registry* registry) {
	this->registry = registry;
}

Planner::~Planner() {}

Graph* Planner::plan(const std::vector<Request>& requests) {
	(void)requests;
	return nullptr;
}
