#pragma once

#include "core/types.h"

class Validator {
public:
	Validator();
	~Validator();

	Graph* validate(Graph *graph);
};


