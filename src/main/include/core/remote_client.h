#pragma once

#include "core/configuration.h"
#include "core/remote_profiles.h"

#include <filesystem>
#include <string>
#include <vector>

int run_remote_client(
    const ReqPackConfig& config,
    const std::filesystem::path& profilePath,
    const std::string& profileName,
    const std::vector<std::string>& forwardedArguments
);
