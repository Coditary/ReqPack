#pragma once

#include "core/execution/orchestrator.h"

#include <filesystem>
#include <string>
#include <vector>

namespace orchestrator_internal {

struct SbomResolutionResult {
	std::vector<Request> requests;
	std::vector<std::string> missingPackages;
};

std::string package_specifier_from_info(const PackageInfo& item);

void cleanup_temp_files(const std::vector<std::filesystem::path>& tempFiles);

bool prepare_requests_for_run(
	std::vector<Request>& requests,
	Registry* registry,
	const ReqPackConfig& config,
	std::vector<std::filesystem::path>& tempFiles
);

void rewrite_registry_package_requests(std::vector<Request>& requests, const RegistryDatabase* database);

bool requests_target_plugin_install(const std::vector<Request>& requests);
bool requests_target_plugin_remove(const std::vector<Request>& requests);

int run_query_action(ActionType action, const std::vector<Request>& requests, Executer* executor);
int run_pack_request(const Request& request, Registry* registry, const ReqPackConfig& config);

std::vector<Request> resolve_audit_requests(Executer* executor, const std::vector<Request>& requests);
std::vector<Request> expand_system_only_audit_requests(Executer* executor, const std::vector<Request>& requests);
SbomResolutionResult resolve_sbom_requests(Executer* executor, const ReqPackConfig& config, const std::vector<Request>& requests);
void log_missing_sbom_packages(const std::vector<std::string>& missingPackages);
void log_validation_blocked(const std::vector<ValidationFinding>& findings);

} // namespace orchestrator_internal
