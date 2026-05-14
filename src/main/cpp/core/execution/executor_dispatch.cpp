#include "core/execution/executor.h"

#include "executor_internal.h"

#include "core/host/host_info.h"
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
