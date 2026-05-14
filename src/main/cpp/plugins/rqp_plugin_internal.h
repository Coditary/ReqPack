#pragma once

#include "plugins/rqp_plugin.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

constexpr const char* BUILTIN_RQP_PLUGIN_ID = "rqp";

IPluginRuntimeHost* rqp_plugin_runtime_host();
void rqp_plugin_configure_runtime_host(const ReqPackConfig* config);
void rqp_plugin_clear_runtime_host_artifacts();
std::vector<std::string> rqp_plugin_take_runtime_host_artifacts();

std::optional<std::filesystem::path> rqp_plugin_unique_nested_file_with_extension(
    const std::filesystem::path& root,
    const std::string& extension
);
