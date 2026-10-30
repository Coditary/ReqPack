#pragma once

#include "cli_parse_state.h"
#include "core/common/types.h"
#include "core/config/configuration.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace cli_internal {

bool uses_unified_package_targets(ActionType action);

/// If `argument` resolves to a readable reqpack.lua manifest, build one Request per system entry.
std::optional<std::vector<Request>> build_requests_from_manifest_argument(
    ActionType action,
    const std::string& argument,
    const std::vector<std::string>& globalFlags,
    const std::function<void(Request&)>& assignOutputOptions,
    CliParseState& state
);

}  // namespace cli_internal
