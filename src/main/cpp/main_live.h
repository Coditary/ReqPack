#pragma once

#include "core/config/configuration.h"
#include "output/logger.h"

#include <filesystem>
#include <string>
#include <vector>

bool is_live_command(const std::vector<std::string>& arguments);
void print_live_run_help(std::ostream& output);
int run_live_command(ReqPackConfig& config,
                     Logger& logger,
                     const std::filesystem::path& configPath,
                     const std::vector<std::string>& rawArguments);
