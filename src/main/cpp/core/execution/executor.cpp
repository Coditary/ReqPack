#include "core/execution/executor.h"

#include "executor_internal.h"

#include <algorithm>
#include <memory>

Executer::Executer(Registry* registry, const ReqPackConfig& config) : config(config), registry(registry), securityGateway(registry, registry, config) {
	this->registry = registry;
	this->transactionDatabase = std::make_unique<TransactionDatabase>(config);
	// Create HistoryManager when at least one tracking feature is active.
	if (config.history.enabled || config.history.trackInstalled) {
		this->historyManager = std::make_unique<HistoryManager>(config);
	}
}

Executer::~Executer() {}

void Executer::setRequestedItemCount(int count, bool inputAlreadyFiltered) const {
	this->requestedItemCount = std::max(0, count);
	this->inputAlreadyFiltered = inputAlreadyFiltered;
}
