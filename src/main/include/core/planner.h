#pragma once

#include "core/types.h"
#include "core/registry.h"

class Planner {
	Registry* registry;

public:
	Planner(Registry* registry);
	~Planner();

	Graph* plan();
};

