#ifndef VALIDATOR_H
#define VALIDATOR_H

#include "core/types.h"

class Validator {
public:
	Validator();
	~Validator();

	Graph* validate(Graph *graph);
};


#endif // VALIDATOR_H
