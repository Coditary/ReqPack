#include "orchestrator_internal.h"

#include "core/download/downloader.h"
#include "core/host/host_info.h"
#include "core/planning/planner_core.h"
#include "core/plugins/plugin_bundle.h"
#include "output/diagnostic.h"
#include "output/logger.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

bool request_targets_plugin_wrapper_refresh(const Request& request) {
	return request.action == ActionType::UPDATE && !request.system.empty() && request.system != "sys" && request.system != "rqp" &&
	       !request.usesLocalTarget && request.packages.empty() &&
	       (std::find(request.flags.begin(), request.flags.end(), "all") == request.flags.end() ||
	        std::find(request.flags.begin(), request.flags.end(), "__reqpack-internal-plugin-refresh-all") != request.flags.end());
}

bool request_targets_system_wide_package_update(const Request& request) {
	return request.action == ActionType::UPDATE && !request.system.empty() && request.system != "sys" &&
	       !request.usesLocalTarget && request.packages.empty() &&
	       std::find(request.flags.begin(), request.flags.end(), "all") != request.flags.end() &&
	       std::find(request.flags.begin(), request.flags.end(), "__reqpack-internal-plugin-refresh-all") == request.flags.end();
}

bool request_targets_plugin_install(const Request& request) {
	return request.action == ActionType::INSTALL && !request.system.empty() && !request.usesLocalTarget && request.packages.empty();
}

bool request_targets_plugin_remove(const Request& request) {
	return request.action == ActionType::REMOVE && !request.system.empty() && request.system != "sys" && request.system != "rqp" &&
	       !request.usesLocalTarget && request.packages.empty();
}

bool plugin_bundle_exists(const ReqPackConfig& config, const std::string& system) {
	const std::filesystem::path pluginPath = std::filesystem::path(config.registry.pluginDirectory) / system;
	return plugin_bundle_read_directory(pluginPath).has_value();
}

std::vector<Package> plugin_bundle_remove_dependencies(const PluginBundleLayout& layout, Registry* registry) {
	std::vector<Package> dependencies;
	for (Package dependency : plugin_bundle_dependency_packages(layout)) {
		dependency = planner_normalize_dependency(std::move(dependency), layout.metadata.name);
		dependency.action = ActionType::REMOVE;
		dependency.system = registry != nullptr ? registry->resolvePluginName(dependency.system) : dependency.system;
		dependencies.push_back(std::move(dependency));
	}
	return dependencies;
}

PluginCallContext plugin_context_for_packages(
	IPlugin* plugin,
	const ReqPackConfig& config,
	const std::vector<std::string>& flags,
	const std::vector<Package>& packages
) {
	std::string itemId;
	if (plugin != nullptr && packages.size() == 1 && !packages.front().system.empty() && !packages.front().name.empty()) {
		itemId = packages.front().system + ":" + packages.front().name;
	}

	return PluginCallContext{
		.pluginId = plugin != nullptr ? plugin->getPluginId() : std::string{},
		.pluginDirectory = plugin != nullptr ? plugin->getPluginDirectory() : std::string{},
		.scriptPath = plugin != nullptr ? plugin->getScriptPath() : std::string{},
		.flags = flags,
		.host = plugin != nullptr ? plugin->getRuntimeHost() : nullptr,
		.proxy = plugin != nullptr ? proxy_config_for_system(config, plugin->getPluginId()) : std::nullopt,
		.currentItemId = itemId,
		.repositories = plugin != nullptr ? repositories_for_ecosystem(config, plugin->getPluginId()) : std::vector<RepositoryEntry>{},
		.hostInfo = HostInfoService::currentSnapshot(),
	};
}

std::vector<Request> expand_system_wide_update_requests(Executer* executor, const std::vector<Request>& requests) {
	std::vector<Request> expanded = requests;
	if (executor == nullptr) {
		return expanded;
	}

	for (Request& request : expanded) {
		if (!request_targets_system_wide_package_update(request)) {
			continue;
		}

		const std::vector<PackageInfo> installedPackages = executor->list(request);
		request.packages.clear();
		request.packages.reserve(installedPackages.size());
		for (const PackageInfo& item : installedPackages) {
			if (item.name.empty()) {
				continue;
			}
			request.packages.push_back(orchestrator_internal::package_specifier_from_info(item));
		}
	}

	return expanded;
}

} // namespace

namespace orchestrator_internal {

bool requests_target_plugin_install(const std::vector<Request>& requests) {
	if (requests.empty()) {
		return false;
	}
	return std::all_of(requests.begin(), requests.end(), [](const Request& request) {
		return request_targets_plugin_install(request);
	});
}

bool requests_target_plugin_remove(const std::vector<Request>& requests) {
	if (requests.empty()) {
		return false;
	}
	return std::all_of(requests.begin(), requests.end(), [](const Request& request) {
		return request_targets_plugin_remove(request);
	});
}

} // namespace orchestrator_internal

bool Orchestrator::shouldRefreshPluginWrappers() const {
	if (this->requests.empty()) {
		return false;
	}
	return std::all_of(this->requests.begin(), this->requests.end(), [](const Request& request) {
		return request_targets_plugin_wrapper_refresh(request);
	});
}

bool Orchestrator::shouldRefreshMainRegistry() const {
	if (this->requests.empty()) {
		return false;
	}
	return std::all_of(this->requests.begin(), this->requests.end(), [](const Request& request) {
		return request.action == ActionType::UPDATE;
	});
}

bool Orchestrator::shouldRunSystemWidePackageUpdates() const {
	if (this->requests.empty()) {
		return false;
	}
	return std::all_of(this->requests.begin(), this->requests.end(), [](const Request& request) {
		return request_targets_system_wide_package_update(request);
	});
}

int Orchestrator::runPluginWrapperRefresh() {
	Logger& logger = Logger::instance();
	std::vector<std::string> itemIds;
	itemIds.reserve(this->requests.size());
	for (const Request& request : this->requests) {
		itemIds.push_back(request.system);
	}

	logger.displaySessionBegin(DisplayMode::UPDATE, itemIds);
	int succeeded = 0;
	int failed = 0;
	for (const Request& request : this->requests) {
		logger.displayItemBegin(request.system, request.system);
		logger.displayItemStep(request.system, "refresh plugin wrapper");
		if (this->registry->refreshPlugin(request.system, true) && this->registry->loadPlugin(request.system)) {
			logger.displayItemSuccess(request.system);
			++succeeded;
			continue;
		}

		logger.displayItemFailure(request.system, make_error_diagnostic(
			"orchestrator",
			"Plugin wrapper refresh failed",
			"ReqPack could not refresh or reload plugin wrapper for requested system.",
			"Check plugin source, registry state, and local plugin cache, then retry update.",
			{},
			request.system,
			"plugin-refresh",
			{{"system", request.system}}
		));
		++failed;
	}

	logger.displaySessionEnd(failed == 0, succeeded, 0, failed);
	return failed == 0 ? 0 : 1;
}

int Orchestrator::runSystemWidePackageUpdates() {
	Logger& logger = Logger::instance();
	std::vector<std::string> itemIds;
	itemIds.reserve(this->requests.size());
	for (const Request& request : this->requests) {
		itemIds.push_back(request.system);
	}

	const std::vector<Request> validationRequests = expand_system_wide_update_requests(this->executor, this->requests);
	const bool hasValidationTargets = std::any_of(validationRequests.begin(), validationRequests.end(), [](const Request& request) {
		return !request.packages.empty();
	});
	if (hasValidationTargets) {
		Graph* graph = this->planner->plan(validationRequests);
		Graph* validatedGraph = this->validator->validate(graph);
		if (validatedGraph == nullptr) {
			if (graph != nullptr) {
				orchestrator_internal::log_validation_blocked(this->validator->getLastFindings());
			}
			delete graph;
			return 1;
		}
		delete validatedGraph;
	}

	logger.displaySessionBegin(DisplayMode::UPDATE, itemIds);
	int succeeded = 0;
	int failed = 0;
	for (const bool ok : this->executor->updateSystems(this->requests)) {
		if (ok) {
			++succeeded;
		} else {
			++failed;
		}
	}

	logger.displaySessionEnd(failed == 0, succeeded, 0, failed);
	return failed == 0 ? 0 : 1;
}

int Orchestrator::runPluginInstallRequests() {
	Logger& logger = Logger::instance();
	std::vector<std::string> itemIds;
	itemIds.reserve(this->requests.size());
	for (const Request& request : this->requests) {
		itemIds.push_back(request.system);
	}

	logger.displaySessionBegin(DisplayMode::INSTALL, itemIds);
	int succeeded = 0;
	int failed = 0;
	Downloader downloader(this->registry->getDatabase(), this->config);
	for (const Request& request : this->requests) {
		logger.displayItemBegin(request.system, request.system);
		logger.displayItemStep(request.system, "install plugin wrapper");
		const std::string resolvedSystem = this->registry->resolvePluginName(request.system);
		if (resolvedSystem.empty()) {
			logger.displayItemFailure(request.system, make_error_diagnostic(
				"install",
				"Plugin install failed",
				"ReqPack could not resolve requested system to a plugin wrapper.",
				"Check plugin name and registry sources, then retry.",
				{},
				request.system,
				"plugin-install"
			));
			++failed;
			continue;
		}

		bool installed = plugin_bundle_exists(this->config, resolvedSystem);
		if (!installed && downloader.downloadPlugin(resolvedSystem)) {
			this->registry->scanDirectory(this->config.registry.pluginDirectory);
			installed = plugin_bundle_exists(this->config, resolvedSystem);
		}
		if (installed) {
			logger.displayItemSuccess(request.system);
			++succeeded;
			continue;
		}

		logger.displayItemFailure(request.system, make_error_diagnostic(
			"install",
			"Plugin install failed",
			"ReqPack could not install requested plugin wrapper from configured local registry sources.",
			"Check registry source path, plugin bundle contents, and auto-download settings, then retry.",
			{},
			request.system,
			"plugin-install"
		));
		++failed;
	}

	logger.displaySessionEnd(failed == 0, succeeded, 0, failed);
	return failed == 0 ? 0 : 1;
}

int Orchestrator::runPluginRemoveRequests() {
	Logger& logger = Logger::instance();
	std::vector<std::string> itemIds;
	itemIds.reserve(this->requests.size());
	for (const Request& request : this->requests) {
		itemIds.push_back(request.system);
	}

	logger.displaySessionBegin(DisplayMode::REMOVE, itemIds);
	int succeeded = 0;
	int failed = 0;
	for (const Request& request : this->requests) {
		logger.displayItemBegin(request.system, request.system);
		logger.displayItemStep(request.system, "remove plugin wrapper");

		const std::string resolvedSystem = this->registry->resolvePluginName(request.system);
		if (resolvedSystem.empty()) {
			logger.displayItemFailure(request.system, make_error_diagnostic(
				"remove",
				"Plugin remove failed",
				"ReqPack could not resolve requested system to an installed plugin wrapper.",
				"Check plugin name and local plugin state, then retry.",
				{},
				request.system,
				"plugin-remove"
			));
			++failed;
			continue;
		}

		const std::optional<PluginBundleLayout> layout = plugin_bundle_find_installed(this->config, resolvedSystem);
		if (!layout.has_value()) {
			logger.displayItemFailure(request.system, make_error_diagnostic(
				"remove",
				"Plugin remove failed",
				"Requested plugin wrapper is not installed in current plugin directory.",
				"Install plugin first or verify configured plugin directory before retrying remove.",
				{},
				request.system,
				"plugin-remove"
			));
			++failed;
			continue;
		}

		if (!this->config.execution.dryRun) {
			std::map<std::string, std::vector<Package>> dependencyPackages;
			for (Package dependency : plugin_bundle_remove_dependencies(layout.value(), this->registry)) {
				if (dependency.system.empty() || dependency.name.empty()) {
					continue;
				}
				dependencyPackages[dependency.system].push_back(std::move(dependency));
			}

			bool dependencyRemovalOk = true;
			std::vector<std::string> dependencyFlags = request.flags;
			dependencyFlags.push_back("__reqpack-internal-silent-runtime");
			for (const auto& [system, packages] : dependencyPackages) {
				if (packages.empty()) {
					continue;
				}

				if (this->registry->getPlugin(system) == nullptr || !this->registry->loadPlugin(system)) {
					dependencyRemovalOk = false;
					break;
				}
				IPlugin* plugin = this->registry->getPlugin(system);
				if (plugin == nullptr) {
					dependencyRemovalOk = false;
					break;
				}

				const PluginCallContext dependencyContext = plugin_context_for_packages(plugin, this->config, dependencyFlags, packages);
				if (!plugin->remove(dependencyContext, packages)) {
					dependencyRemovalOk = false;
					break;
				}
			}

			if (!dependencyRemovalOk) {
				logger.displayItemFailure(request.system, make_error_diagnostic(
					"remove",
					"Plugin remove failed",
					"ReqPack could not remove one or more plugin dependency packages before deleting wrapper files.",
					"Inspect dependency plugin output and retry once dependent packages can be removed cleanly.",
					{},
					request.system,
					"plugin-remove"
				));
				++failed;
				continue;
			}

			this->registry->unloadPlugin(resolvedSystem);
			std::error_code error;
			std::filesystem::remove_all(layout->rootDir, error);
			if (error) {
				logger.displayItemFailure(request.system, make_error_diagnostic(
					"remove",
					"Plugin remove failed",
					"ReqPack could not delete installed plugin wrapper files from disk.",
					"Check filesystem permissions and retry remove after closing any process using that plugin directory.",
					error.message(),
					request.system,
					"plugin-remove"
				));
				++failed;
				continue;
			}

			this->registry->scanDirectory(this->config.registry.pluginDirectory);
		}

		logger.displayItemSuccess(request.system);
		++succeeded;
	}

	logger.displaySessionEnd(failed == 0, succeeded, 0, failed);
	return failed == 0 ? 0 : 1;
}
