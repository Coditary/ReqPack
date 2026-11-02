#pragma once

#include <reqpack/types.h>

#include <string>

namespace cli_internal {

struct CliParseState {
    ActionType& pendingHelpAction;
    std::string& pendingHelpDedicatedCommand;
    bool& lastParseFailed;
    std::string& lastParseError;
};

}  // namespace cli_internal
