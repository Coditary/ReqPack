#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/execution/executor.h"
#include "output/diagnostic.h"
#include "output/idisplay.h"

constexpr const char* INTERNAL_SILENT_RUNTIME_FLAG = "__reqpack-internal-silent-runtime";
constexpr const char* INTERNAL_ENSURE_ORDER_FLAG_PREFIX = "__reqpack-internal-ensure-order=";

inline DiagnosticMessage resolution_failure_diagnostic(const std::string& details, const std::string& scope) {
	return make_error_diagnostic(
		"executor",
		"Request resolution failed",
		"ReqPack could not map requested input to an executable plugin request.",
		"Check system name, package specifier, and flags, then retry.",
		details,
		"executor",
		scope
	);
}

inline DiagnosticMessage system_update_failure_diagnostic(const std::string& system) {
	return make_error_diagnostic(
		"executor",
		"System-wide package update failed",
		"Plugin could not complete update-all operation for requested system.",
		"Inspect plugin output above, verify package manager health, then retry update.",
		{},
		system,
		"update"
	);
}

inline DiagnosticMessage transaction_update_failure_diagnostic(const std::string& system) {
	return make_error_diagnostic(
		"executor",
		"Transaction database update failed",
		"ReqPack could not persist running state for this package operation.",
		"Check transaction database permissions and configured transaction path.",
		{},
		system,
		"transaction"
	);
}

inline DiagnosticMessage plugin_group_failure_diagnostic(const std::string& system,
	                                                     const std::string& summary,
	                                                     const std::string& cause) {
	return make_error_diagnostic(
		"plugin",
		summary,
		cause,
		"Check plugin installation, registry metadata, and package manager health, then retry command.",
		{},
		system,
		"plugin"
	);
}

inline DiagnosticMessage package_failure_diagnostic(const std::string& system,
	                                                const std::string& packageName,
	                                                const bool unavailable) {
	if (unavailable) {
		return make_error_diagnostic(
			"plugin",
			"Package is unavailable: " + system + ":" + packageName,
			"Plugin reported that requested package could not be found or provided by current repositories.",
			"Verify package name, repositories, and system support, then retry.",
			{},
			system + ":" + packageName,
			"package"
		);
	}
	return make_error_diagnostic(
		"plugin",
		"Plugin action failed for " + system + ":" + packageName,
		"Plugin could not complete requested package operation.",
		"Inspect plugin-specific output above and confirm underlying package manager works outside ReqPack.",
		{},
		system + ":" + packageName,
		"package"
	);
}

inline DiagnosticMessage security_gateway_finding_diagnostic(const ValidationFinding& finding) {
	const std::string summary = finding.message.empty() ? (finding.id.empty() ? finding.kind : finding.id) : finding.message;

	if (finding.kind == "sync_warning") {
		return make_warning_diagnostic(
			"security",
			summary,
			"Security gateway could not use preferred refresh path for requested ecosystems.",
			"Check upstream vulnerability feed availability. ReqPack may fall back to a slower full refresh.",
			{},
			finding.source,
			"gateway"
		);
	}

	if (finding.kind == "sync_error") {
		return make_error_diagnostic(
			"security",
			summary,
			"Security gateway could not prepare vulnerability data required for this command.",
			"Check gateway backend configuration, OSV feed access, and local security index permissions.",
			{},
			finding.source,
			"gateway"
		);
	}

	return make_error_diagnostic(
		"security",
		summary,
		"Security gateway returned an unexpected validation finding.",
		"Inspect security gateway configuration and retry command.",
		{},
		finding.source,
		"gateway"
	);
}

inline bool samePackage(const Package& left, const Package& right) {
	return left.action == right.action &&
		left.system == right.system &&
		left.name == right.name &&
		left.version == right.version &&
		left.sourcePath == right.sourcePath &&
		left.localTarget == right.localTarget;
}

inline std::string packageRequestSpec(const Package& package) {
	if (package.version.empty()) {
		return package.name;
	}
	return package.name + "-" + package.version;
}

inline std::string package_item_id(const std::string& system, const Package& package) {
	if (package.version.empty()) {
		return system + ":" + package.name;
	}
	return system + ":" + package.name + "@" + package.version;
}

inline bool actionUsesDesiredStateFilter(const ActionType action) {
	return action == ActionType::INSTALL || action == ActionType::ENSURE || action == ActionType::REMOVE || action == ActionType::UPDATE;
}

inline bool actionUsesMissingPackageFilter(const ActionType action) {
	return action == ActionType::INSTALL || action == ActionType::ENSURE;
}

inline bool actionSupportsRecoveryReconciliation(const ActionType action) {
	return action == ActionType::INSTALL || action == ActionType::ENSURE || action == ActionType::REMOVE || action == ActionType::UPDATE;
}

inline DisplayMode displayModeFromAction(const ActionType action) {
	switch (action) {
		case ActionType::INSTALL: return DisplayMode::INSTALL;
		case ActionType::ENSURE: return DisplayMode::ENSURE;
		case ActionType::REMOVE: return DisplayMode::REMOVE;
		case ActionType::UPDATE: return DisplayMode::UPDATE;
		case ActionType::SEARCH: return DisplayMode::SEARCH;
		case ActionType::LIST: return DisplayMode::LIST;
		case ActionType::INFO: return DisplayMode::INFO;
		case ActionType::OUTDATED: return DisplayMode::OUTDATED;
		case ActionType::SNAPSHOT: return DisplayMode::SNAPSHOT;
		case ActionType::PACK: return DisplayMode::PACK;
		case ActionType::SERVE: return DisplayMode::SERVE;
		case ActionType::REMOTE: return DisplayMode::REMOTE;
		case ActionType::SBOM: return DisplayMode::SBOM;
		default: return DisplayMode::IDLE;
	}
}

inline std::string package_identity_key(const Package& package) {
	return package.system + "\n" + package.name + "\n" + package.version;
}

inline std::string entry_identity_key(const InstalledEntry& entry) {
	return entry.system + "\n" + entry.name + "\n" + entry.version;
}

inline std::vector<std::string> owner_ids_for_package(const Package& package) {
	std::vector<std::string> ownerIds;
	if (package.directRequest) {
		ownerIds.push_back(installed_root_owner_id(package));
	}
	return ownerIds;
}

inline bool is_install_like_action(const ActionType action) {
	return action == ActionType::INSTALL || action == ActionType::ENSURE || action == ActionType::UPDATE;
}

inline bool is_remove_action(const ActionType action) {
	return action == ActionType::REMOVE;
}

inline std::optional<std::size_t> internal_ensure_order(const Package& package) {
	const std::string prefix(INTERNAL_ENSURE_ORDER_FLAG_PREFIX);
	for (const std::string& flag : package.flags) {
		if (flag.rfind(prefix, 0) != 0) {
			continue;
		}
		try {
			return static_cast<std::size_t>(std::stoul(flag.substr(prefix.size())));
		} catch (...) {
			return std::nullopt;
		}
	}
	return std::nullopt;
}
