#pragma once

#include "core/types.h"

class Planner {

public:
	Planner();
	~Planner();

	Graph* plan();
};

