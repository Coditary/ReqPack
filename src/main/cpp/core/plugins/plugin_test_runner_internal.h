#pragma once

#include "core/plugins/plugin_test_runner.h"

namespace plugin_test_runner_internal {

PluginTestRunReport run_plugin_test_cases_impl(const ReqPackConfig& config, const PluginTestInvocation& invocation);

}  // namespace plugin_test_runner_internal
