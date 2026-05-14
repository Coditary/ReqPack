#pragma once

#include "cli/cli.h"
#include "core/config/configuration.h"

#include <string>
#include <vector>

int run_stdin_action_batch(Cli& cli,
                           const ReqPackConfig& config,
                           const std::vector<std::string>& inheritedArguments,
                           ActionType expectedAction,
                           const std::string& actionName);
int run_stdin_serve_loop(Cli& cli,
                         const ReqPackConfig& config,
                         const std::vector<std::string>& inheritedArguments);
