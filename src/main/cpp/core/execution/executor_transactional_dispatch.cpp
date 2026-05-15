#include "core/execution/executor.h"

#include "executor_internal.h"

#include "output/logger.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

std::vector<Executer::TransactionRecord> Executer::executeTransactionalTaskGroup(const TaskGroup& taskGroup, const std::string& runId) const {
	if (taskGroup.usesLocalTarget) {
		if (!this->dispatchTaskGroupToPlugin(taskGroup)) {
			std::vector<TransactionRecord> failureRecords = this->buildFailureRecords(taskGroup);
			for (TransactionRecord& record : failureRecords) {
				record.runId = runId;
				record.errorMessage = "plugin action failed";
			}
			Logger::instance().diagnostic(plugin_group_failure_diagnostic(
				taskGroup.system,
				"Local target install failed for system '" + taskGroup.system + "'",
				"Plugin could not process requested local file or archive target."
			));
			return failureRecords;
		}

		std::vector<TransactionRecord> successRecords = this->buildSuccessRecords(taskGroup);
		for (TransactionRecord& record : successRecords) {
			record.runId = runId;
		}
		return successRecords;
	}

	std::vector<TransactionRecord> records;
	records.reserve(taskGroup.packages.size());

	auto appendSuccessRecords = [&](const std::vector<Package>& packages) {
		if (packages.empty()) {
			return;
		}
		TaskGroup resultTaskGroup = taskGroup;
		resultTaskGroup.packages = packages;
		std::vector<TransactionRecord> successRecords = this->buildSuccessRecords(resultTaskGroup);
		for (TransactionRecord& record : successRecords) {
			record.runId = runId;
		}
		records.insert(records.end(), successRecords.begin(), successRecords.end());
	};

	auto appendFailureRecords = [&](const std::vector<Package>& packages, const std::string& errorMessage) {
		if (packages.empty()) {
			return;
		}
		TaskGroup resultTaskGroup = taskGroup;
		resultTaskGroup.packages = packages;
		std::vector<TransactionRecord> failureRecords = this->buildFailureRecords(resultTaskGroup);
		for (TransactionRecord& record : failureRecords) {
			record.runId = runId;
			record.errorMessage = errorMessage;
		}
		records.insert(records.end(), failureRecords.begin(), failureRecords.end());
	};

	auto appendFailureRecord = [&](const Package& package, const std::string& errorMessage) {
		TaskGroup resultTaskGroup = taskGroup;
		resultTaskGroup.packages = {package};
		std::vector<TransactionRecord> failureRecords = this->buildFailureRecords(resultTaskGroup);
		for (TransactionRecord& record : failureRecords) {
			record.runId = runId;
			record.errorMessage = errorMessage;
		}
		records.insert(records.end(), failureRecords.begin(), failureRecords.end());
	};

	auto displaySuccess = [&](const std::vector<Package>& packages) {
		for (const Package& package : packages) {
			Logger::instance().displayItemSuccess(package_item_id(taskGroup.system, package));
		}
	};

	auto containsPackage = [](const std::vector<Package>& packages, const Package& candidate) {
		return std::any_of(packages.begin(), packages.end(), [&](const Package& package) {
			return samePackage(package, candidate);
		});
	};

	for (const Package& package : taskGroup.packages) {
		Logger::instance().displayItemBegin(package_item_id(taskGroup.system, package), package.name);
	}

	const TaskGroup& executableTaskGroup = taskGroup;

	if (!this->transactionDatabase->updateItemsStatus(runId, executableTaskGroup.packages, "running")) {
		for (const Package& package : executableTaskGroup.packages) {
			Logger::instance().displayItemFailure(package_item_id(taskGroup.system, package),
			                                   transaction_update_failure_diagnostic(taskGroup.system));
		}
		appendFailureRecords(executableTaskGroup.packages, "transaction update failed");
		return records;
	}

	if (this->dispatchTaskGroupToPlugin(executableTaskGroup)) {
		displaySuccess(executableTaskGroup.packages);
		appendSuccessRecords(executableTaskGroup.packages);
		return records;
	}

	std::set<std::string> unavailablePackages;
	if (IPlugin* plugin = this->registry->getPlugin(taskGroup.system); plugin != nullptr) {
		for (const PluginEventRecord& event : plugin->takeRecentEvents()) {
			if (event.name == "unavailable" && !event.payload.empty()) {
				unavailablePackages.insert(event.payload);
			}
		}
	}

	std::vector<Package> succeededPackages;
	std::vector<Package> failedPackages = executableTaskGroup.packages;
    // Only install/ensure can be reconciled via getMissingPackages(). Remove/update need
    // explicit post-state APIs that plugins do not expose today, so keep failures conservative.
    if (actionUsesMissingPackageFilter(taskGroup.action)) {
        IPlugin* plugin = this->registry->getPlugin(taskGroup.system);
        if (plugin != nullptr) {
            const std::vector<Package> remainingMissingPackages = plugin->getMissingPackages(executableTaskGroup.packages);
            succeededPackages.clear();
			failedPackages.clear();
			for (const Package& package : executableTaskGroup.packages) {
				if (containsPackage(remainingMissingPackages, package)) {
					failedPackages.push_back(package);
				} else {
					succeededPackages.push_back(package);
				}
			}
		}
	}

	displaySuccess(succeededPackages);
	appendSuccessRecords(succeededPackages);
	for (const Package& package : failedPackages) {
		const bool unavailable = unavailablePackages.find(packageRequestSpec(package)) != unavailablePackages.end();
		const std::string errorMessage = unavailable ? "package unavailable" : "plugin action failed";
		Logger::instance().displayItemFailure(package_item_id(taskGroup.system, package),
		                                   package_failure_diagnostic(taskGroup.system, package.name, unavailable));
		appendFailureRecord(package, errorMessage);
	}

	return records;
}
