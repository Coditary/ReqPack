#include "cli/cli.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <utility>

namespace {

const std::string PROGRAM_NAME = "ReqPack";
const std::string USAGE = "Usage: ReqPack <command> <system> [packages...] [additional systems/packages...] [flags...]";
const std::string HELP_DESCRIPTION = "Displays this help";

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

}  // namespace

Cli::Cli() : app(std::make_unique<CLI::App>(PROGRAM_NAME + " - Unified Package Manager Interface")) {
    app->name(PROGRAM_NAME);
    app->allow_extras(true);
    app->prefix_command(true);
    app->set_help_flag("-h,--help", HELP_DESCRIPTION);
    app->usage(USAGE);
}

std::vector<Request> Cli::parse(int argc, char* argv[]) {
    return this->parse(argc, argv, DEFAULT_REQPACK_CONFIG);
}

std::vector<Request> Cli::parse(int argc, char* argv[], const ReqPackConfig& config) {
    std::vector<Request> requests;
    std::vector<std::string> global_flags;
    std::unordered_map<std::string, std::size_t> request_index_by_system;

    if (argc < 2) {
        return requests;
    }

    try {
        app->parse(argc, argv);
    } catch (const CLI::ParseError&) {
        return requests;
    }

    const std::vector<std::string> arguments = app->remaining();
    if (arguments.empty()) {
        return requests;
    }

    std::vector<std::string> requestArguments;
    requestArguments.reserve(arguments.size());

    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (is_help_flag(arguments[i])) {
            continue;
        }

        ReqPackConfigOverrides ignoredOverrides;
        std::size_t configIndex = i;
        if (consume_cli_config_flag(arguments, configIndex, ignoredOverrides)) {
            i = configIndex;
            continue;
        }

        requestArguments.push_back(arguments[i]);
    }

    if (requestArguments.empty()) {
        return requests;
    }

    std::size_t actionIndex = requestArguments.size();
    ActionType action = ActionType::UNKNOWN;
    for (std::size_t i = 0; i < requestArguments.size(); ++i) {
        action = parse_action(requestArguments[i]);
        if (action != ActionType::UNKNOWN) {
            actionIndex = i;
            break;
        }
    }

    if (action == ActionType::UNKNOWN) {
        return requests;
    }

    const std::set<std::string> known_systems = discover_systems(config);
    std::string current_system;

    auto ensure_request = [&](const std::string& system) -> Request& {
        const std::string normalized_system = to_lower(system);
        const auto [it, inserted] = request_index_by_system.emplace(normalized_system, requests.size());
        if (inserted) {
            requests.push_back(Request{.action = action, .system = normalized_system});
        }
        return requests[it->second];
    };

    for (std::size_t i = actionIndex + 1; i < requestArguments.size(); ++i) {
        const std::string& argument = requestArguments[i];

        if (is_flag(argument)) {
            global_flags.push_back(argument.substr(2));
            continue;
        }

        const std::optional<std::pair<std::string, std::string>> scoped_package = split_scoped_package(argument, known_systems);
        if (scoped_package.has_value()) {
            Request& request = ensure_request(scoped_package->first);
            request.packages.push_back(scoped_package->second);
            current_system = request.system;
            continue;
        }

        const std::string normalized_argument = to_lower(argument);

        if (current_system.empty()) {
            ensure_request(normalized_argument);
            current_system = normalized_argument;
            continue;
        }

        if (known_systems.contains(normalized_argument)) {
            ensure_request(normalized_argument);
            current_system = normalized_argument;
            continue;
        }

        Request& request = ensure_request(current_system);
        request.packages.push_back(argument);
    }

    for (Request& request : requests) {
        request.flags = global_flags;
    }

    return requests;
}

ReqPackConfigOverrides Cli::parseConfigOverrides(int argc, char* argv[]) const {
    return extract_cli_config_overrides(argc, argv);
}

void Cli::print_help() {
    std::cout << app->help();
    std::cout << "\nConfig:\n"
              << "  --config <path>         Loads config from a custom Lua file\n"
              << "  --config=<path>         Same as above\n";
}

ActionType Cli::parse_action(const std::string& command) {
    const std::string normalized_command = to_lower(command);

    if (normalized_command == "install") {
        return ActionType::INSTALL;
    }
    if (normalized_command == "remove") {
        return ActionType::REMOVE;
    }
    if (normalized_command == "update") {
        return ActionType::UPDATE;
    }
    if (normalized_command == "search") {
        return ActionType::SEARCH;
    }

    return ActionType::UNKNOWN;
}

bool Cli::is_flag(const std::string& argument) {
    return argument.rfind("--", 0) == 0 && argument.size() > 2;
}

bool Cli::is_help_flag(const std::string& argument) {
    return argument == "--help" || argument == "-h";
}

std::optional<std::pair<std::string, std::string>> Cli::split_scoped_package(
    const std::string& argument,
    const std::set<std::string>& known_systems
) {
    const std::size_t separator = argument.find(':');
    if (separator == std::string::npos || separator == 0 || separator == argument.size() - 1) {
        return std::nullopt;
    }

    const std::string system = to_lower(argument.substr(0, separator));
    if (!known_systems.contains(system)) {
        return std::nullopt;
    }

    return std::make_pair(system, argument.substr(separator + 1));
}

std::set<std::string> Cli::discover_systems(const ReqPackConfig& config) {
    std::set<std::string> systems;
    const std::filesystem::path directory = config.registry.pluginDirectory;

    if (!std::filesystem::exists(directory)) {
        for (const auto& [alias, target] : config.planner.systemAliases) {
            systems.insert(to_lower(alias));
            systems.insert(to_lower(target));
        }
        return systems;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".lua") {
            continue;
        }

        systems.insert(to_lower(entry.path().stem().string()));
    }

    for (const auto& [alias, target] : config.planner.systemAliases) {
        systems.insert(to_lower(alias));
        systems.insert(to_lower(target));
    }

    return systems;
}
