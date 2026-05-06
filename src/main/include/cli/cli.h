#pragma once

#include <CLI/CLI.hpp>

#include "core/configuration.h"
#include "core/registry_database.h"
#include "core/types.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class Cli {

public:
    Cli();

    /// Check for -h/--help (optionally preceded or followed by a command).
    /// Prints the appropriate help text to stdout and returns true if help
    /// was requested, so the caller can exit early before any heavy init.
    bool handleHelp(int argc, char* argv[]);

    std::vector<Request> parse(int argc, char* argv[]);
    std::vector<Request> parse(int argc, char* argv[], const ReqPackConfig& config);
    std::vector<Request> parse(const std::vector<std::string>& arguments, const ReqPackConfig& config);
    ReqPackConfigOverrides parseConfigOverrides(int argc, char* argv[]) const;
    bool parseFailed() const;
    const std::string& lastParseError() const;

    void print_help();
    void print_command_help(ActionType action);

private:
    std::unique_ptr<CLI::App> app;
    ActionType pendingHelpAction_ = ActionType::UNKNOWN;
    bool lastParseFailed_ = false;
    std::string lastParseError_;

    static ActionType parse_action(const std::string& command);
    static bool is_flag(const std::string& argument);
    static bool is_help_flag(const std::string& argument);
    static std::optional<std::pair<std::string, std::string>> split_scoped_package(
        const std::string& argument,
        const std::set<std::string>& known_systems
    );
    static std::set<std::string> discover_non_builtin_plugins(const ReqPackConfig& config);
    static std::set<std::string> discover_primary_systems(const ReqPackConfig& config);
    static std::set<std::string> discover_systems(const ReqPackConfig& config);
};
