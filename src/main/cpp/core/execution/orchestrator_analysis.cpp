#include "orchestrator_internal.h"

#include "core/planning/planner_core.h"
#include "output/diagnostic.h"
#include "output/logger.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

bool has_explicit_version(const std::string& packageSpecifier) {
	const std::size_t versionSeparator = packageSpecifier.rfind('@');
	return versionSeparator != std::string::npos && versionSeparator != 0 && versionSeparator + 1 < packageSpecifier.size();
}

DiagnosticMessage validation_blocked_finding_diagnostic(const ValidationFinding& finding) {
	std::string summary = finding.message.empty() ? (finding.id.empty() ? finding.kind : finding.id) : finding.message;
	if (!finding.package.system.empty() && !finding.package.name.empty()) {
		summary += " [" + finding.package.system + ":" + finding.package.name + "]";
	}

	if (finding.kind == "sync_error") {
		return make_error_diagnostic(
			"security",
			summary,
			"ReqPack could not refresh vulnerability data required by current security policy.",
			"Check OSV feed access, local vulnerability database permissions, and retry.",
			{},
			finding.source,
			"sync"
		);
	}

	if (finding.kind == "unsupported_ecosystem") {
		return make_error_diagnostic(
			"security",
			summary,
			"ReqPack has no vulnerability ecosystem mapping for requested package system.",
			"Configure `security.ecosystemMap` or plugin `osvEcosystem`, or relax strict ecosystem mapping if risk is acceptable.",
			{},
			finding.source,
			"mapping"
		);
	}

	if (finding.kind == "unresolved_version") {
		return make_error_diagnostic(
			"security",
			summary,
			"ReqPack could not determine exact package version for vulnerability matching.",
			"Request an explicit version, improve plugin version resolution, or relax unresolved-version policy.",
			{},
			finding.source,
			"version"
		);
	}

	return make_error_diagnostic(
		"security",
		summary,
		"Matched vulnerability data blocks execution under current security policy.",
		"Review advisory details, choose a safe version, or relax security policy only if that risk is acceptable.",
		{},
		finding.source,
		"policy"
	);
}

} // namespace

namespace orchestrator_internal {

std::string package_specifier_from_info(const PackageInfo& item) {
	if (item.version.empty()) {
		return item.name;
	}
	return item.name + '@' + item.version;
}

std::vector<Request> resolve_audit_requests(Executer* executor, const std::vector<Request>& requests) {
	std::vector<Request> resolved = requests;
	if (executor == nullptr) {
		return resolved;
	}

	for (Request& request : resolved) {
		if (request.action != ActionType::AUDIT || request.system.empty() || request.usesLocalTarget || request.packages.empty()) {
			continue;
		}

		std::vector<std::string> resolvedPackages;
		resolvedPackages.reserve(request.packages.size());
		for (const std::string& packageSpecifier : request.packages) {
			if (has_explicit_version(packageSpecifier)) {
				resolvedPackages.push_back(packageSpecifier);
				continue;
			}

			const Package requestedPackage = planner_make_requested_package(request, request.system, packageSpecifier);
			const std::optional<Package> resolvedPackage = executor->resolvePackage(request, requestedPackage);
			if (resolvedPackage.has_value()) {
				resolvedPackages.push_back(planner_package_specifier_from_package(resolvedPackage.value()));
				continue;
			}

			resolvedPackages.push_back(packageSpecifier);
		}
		request.packages = std::move(resolvedPackages);
	}

	return resolved;
}

std::vector<Request> expand_system_only_audit_requests(Executer* executor, const std::vector<Request>& requests) {
	std::vector<Request> expanded = requests;
	if (executor == nullptr) {
		return expanded;
	}

	for (Request& request : expanded) {
		if (request.action != ActionType::AUDIT || request.system.empty() || request.usesLocalTarget || !request.packages.empty()) {
			continue;
		}

		const std::vector<PackageInfo> installedPackages = executor->list(request);
		request.packages.clear();
		request.packages.reserve(installedPackages.size());
		for (const PackageInfo& item : installedPackages) {
			if (item.name.empty()) {
				continue;
			}
			request.packages.push_back(package_specifier_from_info(item));
		}
	}

	return expanded;
}

SbomResolutionResult resolve_sbom_requests(Executer* executor, const ReqPackConfig& config, const std::vector<Request>& requests) {
	SbomResolutionResult result{.requests = requests};
	if (executor == nullptr) {
		return result;
	}

	for (Request& request : result.requests) {
		if (request.action != ActionType::SBOM || request.system.empty() || request.usesLocalTarget) {
			continue;
		}

		if (request.packages.empty()) {
			const std::vector<PackageInfo> installedPackages = executor->list(request);
			request.packages.clear();
			request.packages.reserve(installedPackages.size());
			for (const PackageInfo& item : installedPackages) {
				if (item.name.empty()) {
					continue;
				}
				request.packages.push_back(package_specifier_from_info(item));
			}
		}

		if (request.packages.empty()) {
			continue;
		}

		std::vector<std::string> resolvedPackages;
		resolvedPackages.reserve(request.packages.size());
		for (const std::string& packageSpecifier : request.packages) {
			if (has_explicit_version(packageSpecifier)) {
				resolvedPackages.push_back(packageSpecifier);
				continue;
			}

			const Package requestedPackage = planner_make_requested_package(request, request.system, packageSpecifier);
			const std::optional<Package> resolvedPackage = executor->resolvePackage(request, requestedPackage);
			if (resolvedPackage.has_value()) {
				resolvedPackages.push_back(planner_package_specifier_from_package(resolvedPackage.value()));
				continue;
			}

			if (has_explicit_version(packageSpecifier) || !config.sbom.skipMissingPackages) {
				result.missingPackages.push_back(request.system + ":" + packageSpecifier);
				continue;
			}

			Logger::instance().warn("sbom skipping missing package: " + request.system + ":" + packageSpecifier);
		}
		request.packages = std::move(resolvedPackages);
	}

	return result;
}

void log_missing_sbom_packages(const std::vector<std::string>& missingPackages) {
	for (const std::string& packageSpecifier : missingPackages) {
		Logger::instance().diagnostic(make_error_diagnostic(
			"sbom",
			"sbom missing package: " + packageSpecifier,
			"Requested package is missing, unresolved, or not installed in target system.",
			"Verify package name and version, or rerun with `--sbom-skip-missing-packages` if omission is acceptable.",
			{},
			"sbom",
			"export",
			{{"package", packageSpecifier}}
		));
	}
}

void log_validation_blocked(const std::vector<ValidationFinding>& findings) {
	Logger::instance().diagnostic(make_error_diagnostic(
		"security",
		"execution blocked by security policy",
		"One or more requested packages violated current vulnerability or ecosystem safety policy.",
		"Review reported findings, adjust requested versions, or relax security policy only if that risk is acceptable.",
		{},
		"validator",
		"policy"
	));
	std::size_t shown = 0;
	for (const ValidationFinding& finding : findings) {
		if (finding.kind != "vulnerability" && finding.kind != "sync_error" && finding.kind != "unsupported_ecosystem" &&
			finding.kind != "unresolved_version") {
			continue;
		}
		Logger::instance().diagnostic(validation_blocked_finding_diagnostic(finding));
		++shown;
		if (shown >= 5) {
			break;
		}
	}
}

} // namespace orchestrator_internal
