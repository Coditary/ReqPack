#pragma once

#include "core/config/configuration.h"

#include <optional>
#include <ostream>
#include <string>
#include <vector>

struct PluginTestInvocation {
    std::string plugin;
    std::vector<std::string> presets{};
    std::vector<std::string> caseFiles{};
    std::vector<std::string> caseDirectories{};
    std::optional<std::string> reportPath;
};

struct PluginTestCliParseResult {
    bool matched{false};
    bool helpRequested{false};
    PluginTestInvocation invocation{};
    std::string error{};
};

struct PluginTestCaseSummary {
    std::string name;
    bool success{false};
    std::string message;
    std::vector<std::string> commands{};
    std::vector<std::string> events{};
    std::vector<std::string> stdout{};
    std::vector<std::string> stderr{};
    std::vector<std::string> artifacts{};
    struct EventRecord {
        std::string name;
        std::string payload;
    };
    std::vector<EventRecord> eventRecords{};
};

struct PluginTestRunReport {
    std::string pluginId;
    std::string pluginPath;
    int passed{0};
    int failed{0};
    std::vector<PluginTestCaseSummary> cases{};
};

PluginTestCliParseResult parse_plugin_test_invocation(const std::vector<std::string>& arguments);
void print_plugin_test_help(std::ostream& output);
PluginTestRunReport run_plugin_test_cases(const ReqPackConfig& config, const PluginTestInvocation& invocation);
void print_plugin_test_report(const PluginTestRunReport& report, std::ostream& output);
std::string plugin_test_report_to_json(const PluginTestRunReport& report);
