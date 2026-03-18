#include "core/planner.h"

Planner::Planner(Registry* registry) {
	this->registry = registry;
}

Planner::~Planner() {}

Graph* Planner::plan() {
	return nullptr;
}
