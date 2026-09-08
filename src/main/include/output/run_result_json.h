#pragma once

#include "core/common/types.h"

#include <string>
#include <vector>

struct RunResultJsonItem {
	std::string system;
	std::string name;
	std::string version;
	std::string status;
	std::string errorMessage;
};

struct RunResultJsonDocument {
	bool ok{true};
	bool dryRun{false};
	std::vector<RunResultJsonItem> items;
};

std::string run_result_status_for_action(ActionType action, bool success, bool dryRun);
std::string run_result_status_skipped(ActionType action);
std::string serialize_run_result_json(const RunResultJsonDocument& document);
void emit_run_result_json(const RunResultJsonDocument& document);
