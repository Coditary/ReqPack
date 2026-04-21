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
    std::vector<Request> parse(int argc, char* argv[]);
    std::vector<Request> parse(int argc, char* argv[], const ReqPackConfig& config);
    ReqPackConfigOverrides parseConfigOverrides(int argc, char* argv[]) const;

    void print_help();

private:
    std::unique_ptr<CLI::App> app;

    static ActionType parse_action(const std::string& command);
    static bool is_flag(const std::string& argument);
    static bool is_help_flag(const std::string& argument);
    static std::optional<std::pair<std::string, std::string>> split_scoped_package(
        const std::string& argument,
        const std::set<std::string>& known_systems
    );
    static std::set<std::string> discover_systems(const ReqPackConfig& config);
};
