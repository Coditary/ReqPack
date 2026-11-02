#pragma once

#include <reqpack/config.h>

#include <filesystem>
#include <vector>

class Cli;
class Logger;
class IDisplay;

int dispatch_main_command(Cli& cli,
                          ReqPackConfig& config,
                          const std::filesystem::path& configPath,
                          const ReqPackConfigOverrides& configOverrides,
                          Logger& logger,
                          IDisplay* display,
                          const std::vector<std::string>& rawArguments,
                          int argc,
                          char* argv[]);
