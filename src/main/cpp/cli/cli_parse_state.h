#pragma once

#include "core/common/types.h"

#include <string>

namespace cli_internal {

struct CliParseState {
    ActionType& pendingHelpAction;
    std::string& pendingHelpDedicatedCommand;
    bool& lastParseFailed;
    std::string& lastParseError;
};

}  // namespace cli_internal
