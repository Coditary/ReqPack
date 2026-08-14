#include "core/execution/executor.h"

#include "executor_internal.h"

#include "core/host/host_info.h"
#include "core/planning/planner_platform_policy.h"
#include "output/logger.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

PluginCallContext Executer::buildPluginContext(IPlugin* plugin, const TaskGroup& taskGroup) const {
	if (plugin == nullptr) {
		return {};
	}

	std::string itemId;
	if (taskGroup.usesLocalTarget) {
		itemId = taskGroup.system + ":local";
	} else if (taskGroup.packages.size() == 1) {
		itemId = package_item_id(taskGroup.system, taskGroup.packages.front());
	}

	return PluginCallContext{
		.pluginId = plugin->getPluginId(),
		.pluginDirectory = plugin->getPluginDirectory(),
		.scriptPath = plugin->getScriptPath(),
		.flags = taskGroup.flags,
		.host = plugin->getRuntimeHost(),
		.proxy = proxy_config_for_system(this->config, plugin->getPluginId()),
		.currentItemId = itemId,
		.repositories = repositories_for_ecosystem(this->config, plugin->getPluginId()),
		.hostInfo = HostInfoService::currentSnapshot()
	};
}

std::vector<Executer::TransactionRecord> Executer::executeTaskGroup(const TaskGroup& taskGroup, const std::string& runId) const {
	if (taskGroup.packages.empty() && !taskGroup.usesLocalTarget) {
		return {};
	}

	if (planner_platform::softSkipNixInstalls() &&
	    is_install_like_action(taskGroup.action) &&
	    planner_platform::isNixInstallSystem(this->registry->resolvePluginName(taskGroup.system))) {
		bool consumersReady = !taskGroup.nixSoftSkipConsumers.empty();
		for (const std::string& consumerSystem : taskGroup.nixSoftSkipConsumers) {
			if (this->registry->getPlugin(consumerSystem) == nullptr || !this->registry->loadPlugin(consumerSystem)) {
				consumersReady = false;
				break;
			}
		}
		const bool stillRequired = taskGroup.nixSoftSkipConsumers.empty() || !consumersReady;

		if (stillRequired) {
			std::string packageList;
			for (const Package& package : taskGroup.packages) {
				if (!packageList.empty()) {
					packageList += ", ";
				}
				packageList += package.name;
				if (!package.version.empty()) {
					packageList += "@" + package.version;
				}
			}
			Logger::instance().diagnostic(make_warning_diagnostic(
				"executor",
				"Skipping nix install on Windows",
				"ReqPack will not install nix packages on Windows and expects required tools to already be available on the host.",
				"Install the required tools with a Windows package manager (for example Chocolatey or winget) or manually, then retry.",
				packageList.empty() ? std::string{} : ("packages: " + packageList),
				taskGroup.system,
				"nix-soft-skip"
			));
		}

		// Soft-skip: no plugin call and no success history records.
		return {};
	}

	if (this->securityGateway.isGatewaySystem(taskGroup.system)) {
		const bool ok = this->dispatchTaskGroupToSecurityGateway(taskGroup);
		std::vector<TransactionRecord> records = ok ? this->buildSuccessRecords(taskGroup) : this->buildFailureRecords(taskGroup);
		for (TransactionRecord& record : records) {
			record.runId = runId;
			if (!ok) {
				record.errorMessage = "security gateway action failed";
			}
		}
		return records;
	}

	if (taskGroup.pluginLoadFailed || (taskGroup.plugin == nullptr && (this->registry->getPlugin(taskGroup.system) == nullptr || !this->registry->loadPlugin(taskGroup.system)))) {
		std::vector<TransactionRecord> records = this->buildFailureRecords(taskGroup);
		for (TransactionRecord& record : records) {
			record.runId = runId;
			record.errorMessage = "plugin load failed";
		}
		Logger::instance().diagnostic(plugin_group_failure_diagnostic(
			taskGroup.system,
			"Plugin load failed for system '" + taskGroup.system + "'",
			"ReqPack could not load requested plugin implementation before executing operation."
		));
		return records;
	}

	if (this->config.execution.dryRun) {
		if (taskGroup.usesLocalTarget) {
			const std::string itemId = taskGroup.system + ":local";
			Logger::instance().displayItemBegin(itemId, taskGroup.system);
			Logger::instance().displayItemSuccess(itemId);
		} else {
			for (const Package& package : taskGroup.packages) {
				const std::string itemId = package_item_id(taskGroup.system, package);
				Logger::instance().displayItemBegin(itemId, package.name);
				Logger::instance().displayItemSuccess(itemId);
			}
		}

		std::vector<TransactionRecord> records = this->buildSuccessRecords(taskGroup);
		for (TransactionRecord& record : records) {
			record.runId = runId;
		}
		return records;
	}

	if (this->config.execution.useTransactionDb && this->transactionDatabase != nullptr && !runId.empty()) {
		return this->executeTransactionalTaskGroup(taskGroup, runId);
	}

	if (!this->dispatchTaskGroupToPlugin(taskGroup)) {
		std::vector<TransactionRecord> records = this->buildFailureRecords(taskGroup);
		for (TransactionRecord& record : records) {
			record.runId = runId;
			record.errorMessage = "plugin action failed";
		}
		Logger::instance().diagnostic(plugin_group_failure_diagnostic(
			taskGroup.system,
			"Plugin action failed for system '" + taskGroup.system + "'",
			"Plugin loaded but returned failure while processing requested action."
		));
		return records;
	}

	std::vector<TransactionRecord> records = this->buildSuccessRecords(taskGroup);
	for (TransactionRecord& record : records) {
		record.runId = runId;
	}
	return records;
}

bool Executer::dispatchTaskGroupToPlugin(const TaskGroup& taskGroup) const {
	if (this->securityGateway.isGatewaySystem(taskGroup.system)) {
		return this->dispatchTaskGroupToSecurityGateway(taskGroup);
	}
	IPlugin* plugin = taskGroup.plugin != nullptr ? taskGroup.plugin : this->registry->getPlugin(taskGroup.system);
	if (plugin == nullptr) {
		return false;
	}
	const PluginCallContext context = this->buildPluginContext(plugin, taskGroup);

	switch (taskGroup.action) {
		case ActionType::INSTALL:
		case ActionType::ENSURE:
			if (taskGroup.usesLocalTarget) {
				return plugin->installLocal(context, taskGroup.localPath);
			}
			return plugin->install(context, taskGroup.packages);
		case ActionType::REMOVE:
			return plugin->remove(context, taskGroup.packages);
		case ActionType::UPDATE:
			return plugin->update(context, taskGroup.packages);
		case ActionType::PACK:
			return plugin->supportsPack() && plugin->pack(context, taskGroup.localPath, {}, taskGroup.flags);
		case ActionType::SEARCH:
		case ActionType::LIST:
		case ActionType::INFO:
		case ActionType::UNKNOWN:
		default:
			return false;
	}
}

bool Executer::dispatchTaskGroupToSecurityGateway(const TaskGroup& taskGroup) const {
	const std::vector<ValidationFinding> findings = this->securityGateway.executeGatewayRequest(taskGroup.action, taskGroup.system, taskGroup.packages);
	for (const ValidationFinding& finding : findings) {
		if (finding.kind == "sync_warning") {
			Logger::instance().diagnostic(security_gateway_finding_diagnostic(finding));
		}
		if (finding.kind == "sync_error") {
			Logger::instance().diagnostic(security_gateway_finding_diagnostic(finding));
			return false;
		}
	}
	return true;
}

std::vector<Executer::TransactionRecord> Executer::buildSuccessRecords(const TaskGroup& taskGroup) const {
	std::vector<TransactionRecord> records;
	records.reserve(taskGroup.packages.size());

	for (const Package& package : taskGroup.packages) {
		records.push_back(TransactionRecord{
			.runId = {},
			.system = taskGroup.system,
			.action = taskGroup.action,
			.packageName = package.name,
			.packageVersion = package.version,
			.status = "success"
		});
	}

	return records;
}

std::vector<Executer::TransactionRecord> Executer::buildAlreadySatisfiedRecords(
	const std::vector<TaskGroup>& allTaskGroups,
	const std::vector<TaskGroup>& executableTaskGroups) const
{
	auto packageKey = [](const std::string& system, const std::string& name) {
		return system + '\0' + name;
	};

	std::set<std::string> executableKeys;
	for (const TaskGroup& taskGroup : executableTaskGroups) {
		for (const Package& package : taskGroup.packages) {
			executableKeys.insert(packageKey(taskGroup.system, package.name));
		}
	}

	std::vector<InstalledEntry> installedState;
	if (this->historyManager != nullptr) {
		installedState = this->historyManager->loadInstalledState();
	}

	std::vector<TransactionRecord> records;
	for (const TaskGroup& taskGroup : allTaskGroups) {
		if (!actionUsesMissingPackageFilter(taskGroup.action) || taskGroup.usesLocalTarget) {
			continue;
		}

		for (const Package& package : taskGroup.packages) {
			if (executableKeys.find(packageKey(taskGroup.system, package.name)) != executableKeys.end()) {
				continue;
			}

			std::string version = package.version;
			if (version.empty()) {
				for (const InstalledEntry& entry : installedState) {
					if (entry.system == taskGroup.system && entry.name == package.name) {
						version = entry.version;
						break;
					}
				}
			}

			records.push_back(TransactionRecord{
				.runId = {},
				.system = taskGroup.system,
				.action = taskGroup.action,
				.packageName = package.name,
				.packageVersion = version,
				.status = "success"
			});
		}
	}

	return records;
}

std::vector<Executer::TransactionRecord> Executer::buildFailureRecords(const TaskGroup& taskGroup) const {
	std::vector<TransactionRecord> records;
	records.reserve(taskGroup.packages.size());

	for (const Package& package : taskGroup.packages) {
		records.push_back(TransactionRecord{
			.runId = {},
			.system = taskGroup.system,
			.action = taskGroup.action,
			.packageName = package.name,
			.packageVersion = package.version,
			.status = "failed"
		});
	}

	return records;
}
