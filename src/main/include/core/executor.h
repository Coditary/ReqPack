#ifndef EXECUTOR_H
#define EXECUTOR_H


#include "core/types.h"

class Executer {
public:
	Executer();
	~Executer();

	void execute(Graph *graph);
};


#endif // EXECUTOR_H
