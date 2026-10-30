#pragma once

#include "core/common/types.h"

#include <string>

namespace cli_internal {

bool is_request_output_path_flag(const std::string& argument);
bool is_request_force_flag(const std::string& argument);
bool is_request_all_packages_flag(const std::string& argument);
bool is_request_dry_run_flag(const std::string& argument);
bool is_request_plugins_flag(const std::string& argument);

bool action_supports_force_flag(ActionType action);
bool action_supports_output_path_flag(ActionType action);

/// Maps `-f`, `-o`, `-a`, `-n`, etc. to canonical Request.flags entries (without `--` prefix).
std::optional<std::string> canonical_request_flag_token(ActionType action, const std::string& argument);

bool consume_pack_metadata_flag(const std::vector<std::string>& arguments,
                                std::size_t& index,
                                RqPackMetadataOverrides& overrides);

}  // namespace cli_internal
