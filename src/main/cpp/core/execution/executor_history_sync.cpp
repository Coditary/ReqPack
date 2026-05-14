#include "core/execution/executor.h"

#include "executor_internal.h"

#include "core/plugins/plugin_bundle.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

std::vector<Package> Executer::normalizedRequirements(const Package& package) const {
	if (package.system.empty() || this->securityGateway.isGatewaySystem(package.system)) {
		return {};
	}

	if (const std::optional<PluginBundleLayout> layout = plugin_bundle_find_installed(this->config, package.system); layout.has_value()) {
		std::vector<Package> requirements;
		for (Package dependency : plugin_bundle_dependency_packages(layout.value())) {
			if (dependency.action == ActionType::UNKNOWN) {
				dependency.action = ActionType::INSTALL;
			}
			if (dependency.system.empty()) {
				dependency.system = package.system;
			}
			dependency.system = this->registry->resolvePluginName(dependency.system);
			requirements.push_back(std::move(dependency));
		}
		return requirements;
	}

	if (this->registry->getPlugin(package.system) == nullptr || !this->registry->loadPlugin(package.system)) {
		return {};
	}
	IPlugin* plugin = this->registry->getPlugin(package.system);
	if (plugin == nullptr) {
		return {};
	}

	std::vector<Package> requirements;
	for (Package dependency : plugin->getRequirements()) {
		if (dependency.action == ActionType::UNKNOWN) {
			dependency.action = ActionType::INSTALL;
		}
		if (dependency.system.empty()) {
			dependency.system = package.system;
		}
		dependency.system = this->registry->resolvePluginName(dependency.system);
		requirements.push_back(std::move(dependency));
	}
	return requirements;
}

void Executer::reconcileInstalledOwnership(
	const std::vector<TaskGroup>& allTaskGroups,
	const std::vector<TaskGroup>& plannedTaskGroups,
	const std::vector<TransactionRecord>& records
) const {
	if (this->historyManager == nullptr || !this->config.history.trackInstalled) {
		return;
	}

	std::vector<Package> allPackages = this->collectPackages(allTaskGroups);
	std::vector<Package> plannedPackages = this->collectPackages(plannedTaskGroups);
	auto sameRecordPackage = [](const Package& package, const TransactionRecord& record) {
		return package.action == record.action && package.system == record.system && package.name == record.packageName && package.version == record.packageVersion;
	};
	auto findPackageByRecord = [&](const std::vector<Package>& packages, const TransactionRecord& record) -> const Package* {
		for (const Package& package : packages) {
			if (sameRecordPackage(package, record)) {
				return &package;
			}
		}
		return nullptr;
	};
	auto dependencyMatchesPackage = [](const Package& dependency, const Package& candidate) {
		if (dependency.system != candidate.system || dependency.name != candidate.name) {
			return false;
		}
		return dependency.version.empty() || candidate.version.empty() || dependency.version == candidate.version;
	};
	auto mergeOwnershipForPackage = [&](const Package& package) {
		std::vector<std::string> ownerIds = owner_ids_for_package(package);
		for (const Package& candidate : allPackages) {
			if (candidate.system == package.system && candidate.name == package.name && candidate.version == package.version && candidate.action == package.action) {
				continue;
			}
			for (const Package& dependency : this->normalizedRequirements(candidate)) {
				if (dependencyMatchesPackage(dependency, package)) {
					ownerIds.push_back(installed_package_owner_id(candidate));
				}
			}
		}
		ownerIds.erase(std::remove(ownerIds.begin(), ownerIds.end(), std::string{}), ownerIds.end());
		std::sort(ownerIds.begin(), ownerIds.end());
		ownerIds.erase(std::unique(ownerIds.begin(), ownerIds.end()), ownerIds.end());
		if (!ownerIds.empty()) {
			(void)this->historyManager->mergeInstalledOwnership(package, ownerIds, package.directRequest);
		}

		for (Package dependency : this->normalizedRequirements(package)) {
			const std::vector<std::string> dependencyOwners{installed_package_owner_id(package)};
			(void)this->historyManager->mergeInstalledOwnership(dependency, dependencyOwners, false);
		}
	};
	auto wasPlanned = [&](const Package& package) {
		return std::any_of(plannedPackages.begin(), plannedPackages.end(), [&](const Package& candidate) {
			return samePackage(candidate, package);
		});
	};

	for (const TransactionRecord& record : records) {
		if (record.status != "success" || !is_install_like_action(record.action)) {
			continue;
		}

		const Package* plannedPackage = findPackageByRecord(plannedPackages, record);
		if (plannedPackage == nullptr) {
			continue;
		}
		mergeOwnershipForPackage(*plannedPackage);
	}

	for (const Package& package : allPackages) {
		if (!package.directRequest || !is_install_like_action(package.action) || wasPlanned(package)) {
			continue;
		}
		mergeOwnershipForPackage(package);
	}
}

void Executer::subtractDependencyOwnership(const std::vector<TransactionRecord>& records) const {
	if (this->historyManager == nullptr || !this->config.history.trackInstalled) {
		return;
	}

	for (const TransactionRecord& record : records) {
		if (record.status != "success" || !is_remove_action(record.action)) {
			continue;
		}

		Package removedPackage{
			.action = record.action,
			.system = record.system,
			.name = record.packageName,
			.version = record.packageVersion,
		};
		for (Package dependency : this->normalizedRequirements(removedPackage)) {
			const std::vector<std::string> dependencyOwners{installed_package_owner_id(removedPackage)};
			(void)this->historyManager->subtractInstalledOwnership(dependency, dependencyOwners);
		}
	}
}

std::vector<Executer::TransactionRecord> Executer::removeOrphanedDependencies(
	const std::vector<InstalledEntry>& installedState,
	const std::vector<TransactionRecord>& records
) const {
	if (this->historyManager == nullptr || !this->config.history.trackInstalled) {
		return {};
	}

	auto findInstalledDependency = [&](const Package& dependency) -> const InstalledEntry* {
		const InstalledEntry* singleMatch = nullptr;
		for (const InstalledEntry& entry : installedState) {
			if (entry.system != dependency.system || entry.name != dependency.name) {
				continue;
			}
			if (!dependency.version.empty()) {
				if (entry.version == dependency.version) {
					return &entry;
				}
				continue;
			}
			if (singleMatch != nullptr) {
				return nullptr;
			}
			singleMatch = &entry;
		}
		return singleMatch;
	};

	std::vector<Package> orphanedPackages;
	std::set<std::string> queued;
	for (const TransactionRecord& record : records) {
		if (record.status != "success" || !is_remove_action(record.action)) {
			continue;
		}

		Package removedPackage{
			.action = record.action,
			.system = record.system,
			.name = record.packageName,
			.version = record.packageVersion,
		};
		for (Package dependency : this->normalizedRequirements(removedPackage)) {
			dependency.action = ActionType::REMOVE;
			const InstalledEntry* installedDependency = findInstalledDependency(dependency);
			if (installedDependency == nullptr) {
				continue;
			}
			if (!installedDependency->owners.empty()) {
				continue;
			}
			dependency.version = installedDependency->version;
			const std::string identity = entry_identity_key(*installedDependency);
			if (!queued.insert(identity).second) {
				continue;
			}
			orphanedPackages.push_back(std::move(dependency));
		}
	}

	if (orphanedPackages.empty()) {
		return {};
	}

	std::map<std::string, std::vector<Package>> packagesBySystem;
	for (const Package& package : orphanedPackages) {
		packagesBySystem[package.system].push_back(package);
	}

	std::vector<TransactionRecord> orphanRecords;
	for (const auto& [system, packages] : packagesBySystem) {
		TaskGroup taskGroup{.action = ActionType::REMOVE, .system = system, .packages = packages};
		std::vector<TransactionRecord> batchRecords = this->executeTaskGroup(taskGroup, {});
		orphanRecords.insert(orphanRecords.end(), batchRecords.begin(), batchRecords.end());
	}

	if (!orphanRecords.empty()) {
		this->recordHistory(orphanRecords);
		this->subtractDependencyOwnership(orphanRecords);
		std::vector<InstalledEntry> reloadedState = this->historyManager->loadInstalledState();
		std::vector<TransactionRecord> nested = this->removeOrphanedDependencies(reloadedState, orphanRecords);
		orphanRecords.insert(orphanRecords.end(), nested.begin(), nested.end());
	}

	return orphanRecords;
}

bool Executer::syncInstalledStateForSystem(const std::string& system, const bool allowEmpty) const {
	if (this->historyManager == nullptr || !this->config.history.trackInstalled || system.empty() || this->securityGateway.isGatewaySystem(system)) {
		return false;
	}
	if (this->registry->getPlugin(system) == nullptr || !this->registry->loadPlugin(system)) {
		return false;
	}

	IPlugin* plugin = this->registry->getPlugin(system);
	if (plugin == nullptr) {
		return false;
	}

	TaskGroup taskGroup{.action = ActionType::LIST, .system = system};
	taskGroup.flags = {INTERNAL_SILENT_RUNTIME_FLAG};
	const std::vector<PackageInfo> installedPackages = plugin->list(this->buildPluginContext(plugin, taskGroup));

	std::vector<InstalledEntry> entries;
	entries.reserve(installedPackages.size());
	for (const PackageInfo& item : installedPackages) {
		if (item.name.empty()) {
			continue;
		}
		entries.push_back(InstalledEntry{
			.name = item.name,
			.version = item.version,
			.system = system,
			.installedAt = {},
		});
	}

	if (entries.empty() && !allowEmpty) {
		return false;
	}

	return this->historyManager->replaceInstalledState(system, entries);
}

std::set<std::string> Executer::refreshInstalledState(const std::vector<TransactionRecord>& records) const {
	std::set<std::string> refreshedSystems;
	if (this->historyManager == nullptr || !this->config.history.trackInstalled) {
		return refreshedSystems;
	}

	std::map<std::string, bool> allowEmptyBySystem;
	for (const TransactionRecord& record : records) {
		if (record.status != "success" || !actionUsesDesiredStateFilter(record.action) || record.system.empty()) {
			continue;
		}

		auto [it, inserted] = allowEmptyBySystem.emplace(record.system, true);
		if (record.action != ActionType::REMOVE) {
			it->second = false;
		}
	}

	for (const auto& [system, allowEmpty] : allowEmptyBySystem) {
		if (this->syncInstalledStateForSystem(system, allowEmpty)) {
			refreshedSystems.insert(system);
		}
	}

	return refreshedSystems;
}

void Executer::recordHistory(const std::vector<TransactionRecord>& records) const {
	if (this->historyManager == nullptr) {
		return;
	}

	auto actionToString = [](ActionType action) -> std::string {
		switch (action) {
			case ActionType::INSTALL: return "install";
			case ActionType::ENSURE: return "ensure";
			case ActionType::REMOVE: return "remove";
			case ActionType::UPDATE: return "update";
			default: return "unknown";
		}
	};

	std::vector<HistoryEntry> entries;
	entries.reserve(records.size());
	for (const TransactionRecord& record : records) {
		HistoryEntry entry;
		// timestamp filled in by HistoryManager::record()
		entry.action = actionToString(record.action);
		entry.packageName = record.packageName;
		entry.packageVersion = record.packageVersion;
		entry.system = record.system;
		entry.status = record.status;
		entry.errorMessage = record.errorMessage;
		entries.push_back(entry);
		(void)this->historyManager->appendEvent(entry);
	}

	const std::set<std::string> refreshedSystems = this->refreshInstalledState(records);
	for (const HistoryEntry& entry : entries) {
		const bool isMutatingAction = entry.action == "install" || entry.action == "ensure" || entry.action == "remove" || entry.action == "update";
		if (!isMutatingAction || refreshedSystems.find(entry.system) != refreshedSystems.end()) {
			continue;
		}
		(void)this->historyManager->updateInstalledState(entry);
	}
}
