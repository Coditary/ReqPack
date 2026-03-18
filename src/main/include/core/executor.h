#pragma once

#include "core/types.h"

class Executer {
public:
	Executer();
	~Executer();

	void execute(Graph *graph);
};

