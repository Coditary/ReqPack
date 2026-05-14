#pragma once

#include "core/remote/serve_remote.h"

#include <string>
#include <vector>

struct RemoteClientInvocation {
    std::string profileName;
    std::vector<std::string> forwardedArguments;
};

bool parse_serve_runtime_options(const std::vector<std::string>& arguments,
                                 ServeRuntimeOptions& options,
                                 std::string& error);
bool parse_remote_client_invocation(const std::vector<std::string>& arguments,
                                    RemoteClientInvocation& invocation,
                                    std::string& error);
bool is_action_stdin_command(const std::vector<std::string>& arguments, const std::string& action);
std::vector<std::string> inherited_stream_arguments(const std::vector<std::string>& arguments);
bool is_self_update_command(const std::vector<std::string>& arguments);
bool is_host_refresh_command(const std::vector<std::string>& arguments);
bool is_version_command(const std::vector<std::string>& arguments);
bool is_info_command(const std::vector<std::string>& arguments);
