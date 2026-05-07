#pragma once

#include "core/config/configuration.h"
#include "core/remote/remote_profiles.h"
#include "output/idisplay.h"

#include <filesystem>
#include <string>
#include <vector>

int run_remote_client(
    const ReqPackConfig& config,
    const std::filesystem::path& profilePath,
    const std::string& profileName,
    const std::vector<std::string>& forwardedArguments,
    IDisplay* display = nullptr
);
