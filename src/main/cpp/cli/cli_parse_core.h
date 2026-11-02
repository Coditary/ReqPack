#pragma once

#include <CLI/CLI.hpp>

#include "cli_parse_state.h"
#include <reqpack/config.h>

#include <string>
#include <vector>

namespace cli_internal {

std::vector<Request> parse_argv(CLI::App& app, int argc, char* argv[], const ReqPackConfig& config, CliParseState state);
std::vector<Request> parse_arguments(const std::vector<std::string>& arguments, const ReqPackConfig& config, CliParseState state);

}  // namespace cli_internal
