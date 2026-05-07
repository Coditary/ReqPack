#pragma once

#include "cli/cli.h"
#include "core/config/configuration.h"
#include "output/idisplay.h"
#include "output/command_output.h"
#include "output/logger.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

enum class ServeRemoteProtocol {
    TEXT,
    JSON,
    HTTP,
    HTTPS
};

struct ServeRuntimeOptions {
    bool stdin{false};
    bool remote{false};
    ServeRemoteProtocol remoteProtocol{ServeRemoteProtocol::TEXT};
    std::string bind{"127.0.0.1"};
    int port{4545};
    std::optional<std::string> token;
    std::optional<std::string> username;
    std::optional<std::string> password;
    bool readonly{false};
    bool readonlyExplicit{false};
    int maxConnections{16};
    bool maxConnectionsExplicit{false};
    std::vector<std::string> inheritedArguments;
};

int run_remote_serve(
    Cli& cli,
    const ReqPackConfig& config,
    const std::filesystem::path& configPath,
    const ReqPackConfigOverrides& configOverrides,
    Logger& logger,
    IDisplay* display,
    const ServeRuntimeOptions& options
);

bool remote_protocol_requires_explicit_mode(ServeRemoteProtocol protocol);
