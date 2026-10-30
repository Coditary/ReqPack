#include "cli_plugin_commands.h"

#include "cli_parse_core.h"
#include "cli_parse_shared.h"
#include "cli_system_discovery.h"

#include <algorithm>

namespace cli_internal {

namespace {

bool matches_token(const std::string& value, const std::string& token) {
    return to_lower_copy(value) == to_lower_copy(token);
}

std::vector<std::string> collect_plugin_update_flags_and_systems(
    const std::vector<std::string>& requestArguments,
    std::size_t startIndex,
    std::vector<std::string>& globalFlags,
    std::vector<std::string>& systems,
    CliParseState& state
) {
    for (std::size_t i = startIndex; i < requestArguments.size(); ++i) {
        const std::string& argument = requestArguments[i];
        if (is_help_flag_argument(argument)) {
            continue;
        }
        if (is_flag_argument(argument)) {
            if (argument.rfind("--", 0) == 0) {
                globalFlags.push_back(argument.substr(2));
            }
            continue;
        }
        systems.push_back(to_lower_copy(argument));
    }
    (void)state;
    return globalFlags;
}

std::vector<Request> build_plugin_wrapper_update_requests(
    const ReqPackConfig& config,
    const std::vector<std::string>& globalFlags,
    const std::vector<std::string>& systems,
    bool refreshAll
) {
    std::vector<Request> requests;
    if (refreshAll && systems.empty()) {
        const std::vector<std::string> installedPlugins = discover_installed_plugins(config);
        if (!installedPlugins.empty()) {
            for (const std::string& plugin : installedPlugins) {
                Request request {.action = ActionType::UPDATE, .system = plugin, .flags = globalFlags};
                request.flags.push_back("__reqpack-internal-plugin-refresh-all");
                requests.push_back(std::move(request));
            }
            return requests;
        }

        Request request {.action = ActionType::UPDATE, .flags = globalFlags};
        request.flags.push_back("all");
        request.flags.push_back("__reqpack-internal-plugin-refresh-all");
        request.flags.push_back("__reqpack-internal-update-all-expand");
        requests.push_back(std::move(request));
        return requests;
    }

    const std::set<std::string> knownSystems = discover_systems(config);
    for (const std::string& system : systems) {
        if (!knownSystems.contains(system)) {
            continue;
        }
        Request request {.action = ActionType::UPDATE, .system = system, .flags = globalFlags};
        request.flags.push_back("__reqpack-internal-plugin-refresh-all");
        requests.push_back(std::move(request));
    }

    if (requests.empty() && !systems.empty()) {
        return {};
    }

    return requests;
}

}  // namespace

bool is_plugin_cluster_command(const std::vector<std::string>& arguments) {
    for (const std::string& argument : arguments) {
        if (is_help_flag_argument(argument)) {
            continue;
        }
        ReqPackConfigOverrides ignored;
        std::size_t index = 0;
        if (consume_cli_config_flag(arguments, index, ignored)) {
            continue;
        }
        return matches_token(argument, "plugin");
    }
    return false;
}

std::optional<std::vector<Request>> try_parse_plugin_cluster(
    const std::vector<std::string>& requestArguments,
    const ReqPackConfig& config,
    CliParseState& state
) {
    if (requestArguments.empty() || !matches_token(requestArguments.front(), "plugin")) {
        return std::nullopt;
    }

    if (requestArguments.size() < 2) {
        state.lastParseFailed = true;
        state.lastParseError = "plugin requires a subcommand (update, ensure, test, info)";
        return std::vector<Request> {};
    }

    const std::string subcommand = to_lower_copy(requestArguments[1]);

    if (subcommand == "ensure" || subcommand == "info") {
        std::vector<std::string> rewritten(requestArguments.begin() + 1, requestArguments.end());
        return parse_arguments(rewritten, config, state);
    }

    if (subcommand == "update" || subcommand == "update-tool") {
        std::vector<std::string> globalFlags;
        std::vector<std::string> systems;
        collect_plugin_update_flags_and_systems(requestArguments, 2, globalFlags, systems, state);

        const bool explicitAll = std::find(globalFlags.begin(), globalFlags.end(), "all") != globalFlags.end();
        globalFlags.erase(
            std::remove(globalFlags.begin(), globalFlags.end(), "all"),
            globalFlags.end());

        if (subcommand == "update-tool") {
            if (systems.size() != 1) {
                state.lastParseFailed = true;
                state.lastParseError = "plugin update-tool requires exactly one system argument";
                return std::vector<Request> {};
            }
            Request request {.action = ActionType::UPDATE, .system = "sys", .packages = {systems.front()}, .flags = globalFlags};
            return std::vector<Request> {std::move(request)};
        }

        const bool refreshAll = explicitAll || systems.empty();
        std::vector<Request> requests = build_plugin_wrapper_update_requests(config, globalFlags, systems, refreshAll);
        if (requests.empty() && !systems.empty()) {
            state.lastParseFailed = true;
            state.lastParseError = "unknown plugin system for plugin update";
            return std::vector<Request> {};
        }
        return requests;
    }

    state.lastParseFailed = true;
    state.lastParseError = "unknown plugin subcommand: " + subcommand;
    return std::vector<Request> {};
}

}  // namespace cli_internal
