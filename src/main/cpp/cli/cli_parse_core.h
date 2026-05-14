#pragma once

#include <CLI/CLI.hpp>

#include "core/common/types.h"
#include "core/config/configuration.h"

#include <string>
#include <vector>

namespace cli_internal {

struct CliParseState {
    ActionType& pendingHelpAction;
    bool& lastParseFailed;
    std::string& lastParseError;
};

std::vector<Request> parse_argv(CLI::App& app, int argc, char* argv[], const ReqPackConfig& config, CliParseState state);
std::vector<Request> parse_arguments(const std::vector<std::string>& arguments, const ReqPackConfig& config, CliParseState state);

}  // namespace cli_internal
