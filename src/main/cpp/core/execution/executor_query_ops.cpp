#include "core/execution/executor.h"

#include "executor_internal.h"

#include "core/planning/request_resolution.h"
#include "output/logger.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

std::string normalize_search_filter_value(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return value;
}

struct PackageResultFilters {
	std::set<std::string> architectures;
	std::set<std::string> packageTypes;
};

PackageResultFilters parse_package_result_filters(const std::vector<std::string>& flags) {
	PackageResultFilters filters;
	for (const auto& flag : flags) {
		if (flag.rfind("arch=", 0) == 0 && flag.size() > 5) {
			filters.architectures.insert(normalize_search_filter_value(flag.substr(5)));
		}
		if (flag.rfind("type=", 0) == 0 && flag.size() > 5) {
			filters.packageTypes.insert(normalize_search_filter_value(flag.substr(5)));
		}
	}
	return filters;
}

bool matches_package_result_filters(const PackageInfo& info, const PackageResultFilters& filters) {
	if (!filters.architectures.empty()) {
		if (info.architecture.empty() || !filters.architectures.contains(normalize_search_filter_value(info.architecture))) {
			return false;
		}
	}
	if (!filters.packageTypes.empty()) {
		if (info.packageType.empty() || !filters.packageTypes.contains(normalize_search_filter_value(info.packageType))) {
			return false;
		}
	}
	return true;
}

std::vector<PackageInfo> apply_package_result_filters(std::vector<PackageInfo> results, const std::vector<std::string>& flags) {
	const PackageResultFilters filters = parse_package_result_filters(flags);
	if (filters.architectures.empty() && filters.packageTypes.empty()) {
		return results;
	}
	results.erase(std::remove_if(results.begin(), results.end(), [&](const PackageInfo& info) {
		return !matches_package_result_filters(info, filters);
	}), results.end());
	return results;
}

std::vector<PackageInfo> apply_requested_package_subset(std::vector<PackageInfo> results, const std::vector<std::string>& packageSpecifiers) {
	if (packageSpecifiers.empty()) {
		return results;
	}

	std::set<std::string> requestedNames;
	for (const std::string& specifier : packageSpecifiers) {
		const std::size_t versionSeparator = specifier.find('@');
		requestedNames.insert(specifier.substr(0, versionSeparator));
	}

	results.erase(std::remove_if(results.begin(), results.end(), [&](const PackageInfo& info) {
		return info.name.empty() || !requestedNames.contains(info.name);
	}), results.end());
	return results;
}

}  // namespace

std::vector<PackageInfo> Executer::list(const Request& request) const {
	std::string resolutionError;
	const std::optional<Request> resolvedRequest = this->resolveRequest(request, &resolutionError);
	if (!resolvedRequest.has_value()) {
		if (!resolutionError.empty()) {
			Logger::instance().err(resolutionError);
		}
		return {};
	}
	if (this->registry->getPlugin(resolvedRequest->system) == nullptr || !this->registry->loadPlugin(resolvedRequest->system)) {
		return {};
	}
	IPlugin* plugin = this->registry->getPlugin(resolvedRequest->system);
	TaskGroup taskGroup{.action = ActionType::LIST, .system = resolvedRequest->system};
	taskGroup.flags = resolvedRequest->flags;
	std::vector<PackageInfo> results = plugin->list(this->buildPluginContext(plugin, taskGroup));
	results = apply_requested_package_subset(std::move(results), resolvedRequest->packages);
	return apply_package_result_filters(std::move(results), resolvedRequest->flags);
}

std::vector<PackageInfo> Executer::outdated(const Request& request) const {
	std::string resolutionError;
	const std::optional<Request> resolvedRequest = this->resolveRequest(request, &resolutionError);
	if (!resolvedRequest.has_value()) {
		if (!resolutionError.empty()) {
			Logger::instance().err(resolutionError);
		}
		return {};
	}
	if (this->registry->getPlugin(resolvedRequest->system) == nullptr || !this->registry->loadPlugin(resolvedRequest->system)) {
		return {};
	}
	IPlugin* plugin = this->registry->getPlugin(resolvedRequest->system);
	TaskGroup taskGroup{.action = ActionType::OUTDATED, .system = resolvedRequest->system};
	taskGroup.flags = resolvedRequest->flags;
	std::vector<PackageInfo> results = plugin->outdated(this->buildPluginContext(plugin, taskGroup));
	results = apply_requested_package_subset(std::move(results), resolvedRequest->packages);
	return apply_package_result_filters(std::move(results), resolvedRequest->flags);
}

std::vector<PackageInfo> Executer::search(const Request& request) const {
	std::string resolutionError;
	const std::optional<Request> resolvedRequest = this->resolveRequest(request, &resolutionError);
	if (!resolvedRequest.has_value()) {
		if (!resolutionError.empty()) {
			Logger::instance().err(resolutionError);
		}
		return {};
	}
	if (this->registry->getPlugin(resolvedRequest->system) == nullptr || !this->registry->loadPlugin(resolvedRequest->system)) {
		return {};
	}
	IPlugin* plugin = this->registry->getPlugin(resolvedRequest->system);
	TaskGroup taskGroup{.action = ActionType::SEARCH, .system = resolvedRequest->system};
	taskGroup.flags = resolvedRequest->flags;
	std::string prompt;
	for (std::size_t index = 0; index < resolvedRequest->packages.size(); ++index) {
		if (index > 0) {
			prompt += ' ';
		}
		prompt += resolvedRequest->packages[index];
	}
	std::vector<PackageInfo> results = plugin->search(this->buildPluginContext(plugin, taskGroup), prompt);
	return apply_package_result_filters(std::move(results), resolvedRequest->flags);
}

PackageInfo Executer::info(const Request& request) const {
	std::string resolutionError;
	const std::optional<Request> resolvedRequest = this->resolveRequest(request, &resolutionError);
	if (!resolvedRequest.has_value()) {
		if (!resolutionError.empty()) {
			Logger::instance().err(resolutionError);
		}
		return {};
	}
	if (this->registry->getPlugin(resolvedRequest->system) == nullptr || !this->registry->loadPlugin(resolvedRequest->system)) {
		return {};
	}
	IPlugin* plugin = this->registry->getPlugin(resolvedRequest->system);
	TaskGroup taskGroup{.action = ActionType::INFO, .system = resolvedRequest->system};
	taskGroup.flags = resolvedRequest->flags;
	const std::string packageName = resolvedRequest->packages.empty() ? std::string{} : resolvedRequest->packages.front();
	return plugin->info(this->buildPluginContext(plugin, taskGroup), packageName);
}

std::optional<Package> Executer::resolvePackage(const Request& request, const Package& package) const {
	std::string resolutionError;
	const std::optional<Request> resolvedRequest = this->resolveRequest(request, &resolutionError);
	if (!resolvedRequest.has_value()) {
		if (!resolutionError.empty()) {
			Logger::instance().err(resolutionError);
		}
		return std::nullopt;
	}
	if (this->registry->getPlugin(resolvedRequest->system) == nullptr || !this->registry->loadPlugin(resolvedRequest->system)) {
		return std::nullopt;
	}
	IPlugin* plugin = this->registry->getPlugin(resolvedRequest->system);
	TaskGroup taskGroup{.action = ActionType::SBOM, .system = resolvedRequest->system};
	taskGroup.flags = resolvedRequest->flags;
	Package resolvedPackage = package;
	resolvedPackage.system = resolvedRequest->system;
	if (plugin->supportsResolvePackage()) {
		taskGroup.flags.push_back(INTERNAL_SILENT_RUNTIME_FLAG);
		if (const std::optional<Package> resolved = plugin->resolvePackage(this->buildPluginContext(plugin, taskGroup), resolvedPackage); resolved.has_value()) {
			return resolved;
		}
		return std::nullopt;
	}

	if (!resolvedPackage.version.empty()) {
		return resolvedPackage;
	}

	Request infoRequest = resolvedRequest.value();
	infoRequest.action = ActionType::INFO;
	infoRequest.packages = {resolvedPackage.name};
	infoRequest.flags.push_back(INTERNAL_SILENT_RUNTIME_FLAG);
	const PackageInfo info = this->info(infoRequest);
	if (!info.name.empty() && !info.version.empty() && info.version != "unknown" && info.version != "repo" && info.version != "installed") {
		Package resolved = resolvedPackage;
		resolved.name = info.name;
		resolved.version = info.version;
		return resolved;
	}
	if (info.name.empty() && info.version.empty() && info.description.empty()) {
		return std::nullopt;
	}

	return resolvedPackage;
}

std::optional<Request> Executer::resolveRequest(const Request& request, std::string* errorMessage) const {
	RequestResolutionService resolver(this->registry, this->config);
	return resolver.resolveRequest(request, errorMessage);
}
