#ifndef PLANNER_H
#define PLANNER_H

#include "core/types.h"

class Planner {

public:
	Planner();
	~Planner();

	Graph* plan();
};


#endif // PLANNER_H
