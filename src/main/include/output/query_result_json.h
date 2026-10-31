#pragma once

#include "core/common/types.h"
#include "core/config/configuration.h"
#include "output/run_result_json.h"

#include <string>
#include <vector>

RunResultJsonDocument build_query_packages_document(ActionType action, const std::vector<PackageInfo>& items);

void emit_query_packages_json(ActionType action, const std::vector<PackageInfo>& items);

void emit_query_info_json(const Request& request, const PackageInfo& item, bool success);

void emit_local_rqp_info_json(const ReqPackConfig& config, const std::string& version);
