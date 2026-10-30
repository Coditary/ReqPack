#pragma once

#include "cli_parse_state.h"
#include "core/config/configuration.h"

#include <optional>
#include <string>
#include <vector>

namespace cli_internal {

/// Parses `rqp plugin …` and returns orchestrator requests, or nullopt if not a plugin cluster invocation.
std::optional<std::vector<Request>> try_parse_plugin_cluster(
    const std::vector<std::string>& requestArguments,
    const ReqPackConfig& config,
    CliParseState& state
);

bool is_plugin_cluster_command(const std::vector<std::string>& arguments);

}  // namespace cli_internal
