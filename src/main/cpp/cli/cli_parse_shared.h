#pragma once

#include "core/common/types.h"
#include "core/config/configuration.h"

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace cli_internal {

std::string to_lower_copy(std::string value);
ActionType parse_action_token(const std::string& command);
bool is_flag_argument(const std::string& argument);
bool is_help_flag_argument(const std::string& argument);

std::optional<std::pair<std::string, std::string>> split_scoped_package_argument(
    const std::string& argument,
    const std::set<std::string>& known_systems
);

bool is_existing_path(const std::string& value);
bool is_existing_regular_file(const std::string& value);
bool is_url(const std::string& value);
bool supports_manifest_path(ActionType action);

std::optional<std::filesystem::path> resolve_manifest_path_argument(const std::string& argument);
std::optional<AuditOutputFormat> infer_audit_output_format_from_path(const std::string& path);

bool consume_package_result_filter_flag(
    ActionType action,
    const std::vector<std::string>& arguments,
    std::size_t& index,
    std::vector<std::string>& flags
);

bool has_flag(const std::vector<std::string>& flags, const std::string& name);
bool update_command_has_package_mode_flag(const std::vector<std::string>& arguments);
bool is_removed_security_backend_flag(const std::string& argument);
bool current_system_prefers_package_tokens(const std::string& currentSystem, ActionType action);

}  // namespace cli_internal
