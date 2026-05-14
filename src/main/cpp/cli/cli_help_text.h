#pragma once

#include "core/common/types.h"

#include <string>
#include <string_view>
#include <vector>

namespace cli_internal {

struct HelpScanResult {
    bool hasHelp{false};
    ActionType action{ActionType::UNKNOWN};
};

std::string_view program_name();
std::string_view usage_text();
std::string_view help_description();
std::string_view verbose_description();

HelpScanResult scan_help_arguments(const std::vector<std::string>& arguments);
std::string general_help_text();
std::string command_help_text(ActionType action);

}  // namespace cli_internal
