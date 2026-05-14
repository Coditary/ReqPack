#pragma once

#include "core/host/host_info.h"

#include <optional>
#include <string>

namespace host_info_internal {

std::string trim_copy(const std::string& value);
std::string to_lower_copy(std::string value);
std::optional<std::string> optional_trimmed(const std::string& value);
std::int64_t current_epoch_seconds();
HostInfoSnapshot collect_live_snapshot(const std::string& refreshReason);

}  // namespace host_info_internal
