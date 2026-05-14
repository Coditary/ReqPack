#include "cli_parse_core.h"

#include "cli_help_text.h"
#include "cli_parse_shared.h"
#include "cli_system_discovery.h"

#include "core/manifest/manifest_loader.h"
#include "output/diagnostic.h"
#include "output/logger.h"

#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <utility>

namespace {

DiagnosticMessage manifest_missing_diagnostic(const std::filesystem::path& manifestPath) {
    return make_error_diagnostic(
        "cli",
        "Manifest not found: " + MANIFEST_FILENAME + " missing in '" + manifestPath.parent_path().string() + "'",
        "Install command was given a directory path, but that directory does not contain a reqpack.lua manifest.",
        "Create reqpack.lua in that directory or pass packages directly instead of a manifest path.",
        {},
        "cli",
        "manifest",
        {{"path", manifestPath.parent_path().string()}}
    );
}

DiagnosticMessage manifest_load_diagnostic(const std::filesystem::path& manifestPath, const std::string& details) {
    return make_error_diagnostic(
        "cli",
        "Manifest load failed: '" + manifestPath.string() + "'",
        "Manifest file exists but could not be parsed or executed.",
        "Check Lua syntax and manifest structure, then run command again.",
        details,
        "cli",
        "manifest",
        {{"path", manifestPath.string()}}
    );
}

DiagnosticMessage manifest_empty_diagnostic(const std::filesystem::path& manifestPath) {
    return make_error_diagnostic(
        "cli",
        "Manifest contains no packages: '" + manifestPath.string() + "'",
        "reqpack.lua loaded successfully but returned no installable package entries.",
        "Add entries under packages = { ... } or use direct package arguments.",
        {},
        "cli",
        "manifest",
        {{"path", manifestPath.string()}}
    );
}

DiagnosticMessage mixed_local_and_package_diagnostic(const std::string& system) {
    return make_error_diagnostic(
        "cli",
        "Install input is ambiguous for system '" + system + "'",
        "Local path targets and package names were provided in same request for one system.",
        "Use either package names or one local path per system in a single install command.",
        {},
        system,
        "install",
        {{"system", system}}
    );
}

DiagnosticMessage unknown_search_system_diagnostic(const std::string& system) {
    return make_error_diagnostic(
        "cli",
        "Unknown search system: '" + system + "'",
        "Search requires one explicit known system before query terms.",
        "Use `rqp search <system> <query>` with installed or registered plugin name.",
        {},
        "cli",
        "search",
        {{"system", system}}
    );
}

void reset_parse_state(cli_internal::CliParseState state) {
    state.pendingHelpAction = ActionType::UNKNOWN;
    state.lastParseFailed = false;
    state.lastParseError.clear();
}

}  // namespace

namespace cli_internal {

std::vector<Request> parse_argv(CLI::App& app, int argc, char* argv[], const ReqPackConfig& config, CliParseState state) {
    reset_parse_state(state);
    if (argc < 2) {
        return {};
    }

    std::vector<std::string> rawArguments;
    rawArguments.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) {
        rawArguments.emplace_back(argv[i]);
    }

    const HelpScanResult helpScan = scan_help_arguments(rawArguments);
    if (helpScan.hasHelp) {
        state.pendingHelpAction = helpScan.action;
        return {};
    }

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError&) {
        state.lastParseFailed = true;
        return {};
    }

    return parse_arguments(app.remaining(), config, state);
}

std::vector<Request> parse_arguments(const std::vector<std::string>& arguments, const ReqPackConfig& config, CliParseState state) {
    reset_parse_state(state);

    std::vector<Request> requests;
    std::vector<std::string> global_flags;
    std::unordered_map<std::string, std::size_t> request_index_by_system;
    std::string sbomOutputFormat;
    std::string sbomOutputPath;
    std::string auditOutputFormat;
    std::string auditOutputPath;
    std::string snapshotOutputPath;
    std::string packOutputPath;
    std::string packPayloadPath;

    if (arguments.empty()) {
        return requests;
    }

    const HelpScanResult helpScan = scan_help_arguments(arguments);
    if (helpScan.hasHelp) {
        state.pendingHelpAction = helpScan.action;
        return requests;
    }

    std::vector<std::string> requestArguments;
    requestArguments.reserve(arguments.size());

    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (is_help_flag_argument(arguments[i])) {
            continue;
        }

        ReqPackConfigOverrides ignoredOverrides;
        std::size_t configIndex = i;
        if (consume_cli_config_flag(arguments, configIndex, ignoredOverrides)) {
            if (ignoredOverrides.errorMessage.has_value()) {
                state.lastParseFailed = true;
                state.lastParseError = ignoredOverrides.errorMessage.value();
                return {};
            }
            i = configIndex;
            continue;
        }

        requestArguments.push_back(arguments[i]);
    }

    if (requestArguments.empty()) {
        return requests;
    }

    if (!requestArguments.empty() &&
        to_lower_copy(requestArguments.front()) == to_lower_copy(std::string(program_name()))) {
        requestArguments.erase(requestArguments.begin());
        if (requestArguments.empty()) {
            return requests;
        }
    }

    std::size_t actionIndex = requestArguments.size();
    ActionType action = ActionType::UNKNOWN;
    for (std::size_t i = 0; i < requestArguments.size(); ++i) {
        action = parse_action_token(requestArguments[i]);
        if (action != ActionType::UNKNOWN) {
            actionIndex = i;
            break;
        }
    }

    if (action == ActionType::UNKNOWN) {
        state.lastParseFailed = true;
        return requests;
    }

    if (std::any_of(requestArguments.begin(), requestArguments.end(), [](const std::string& argument) {
            return is_removed_security_backend_flag(argument);
        })) {
        state.lastParseFailed = true;
        return {};
    }

    if (action == ActionType::REMOTE || action == ActionType::HOST) {
        return requests;
    }

    if (action == ActionType::PACK) {
        std::vector<std::string> positional;
        positional.reserve(requestArguments.size());
        for (std::size_t i = actionIndex + 1; i < requestArguments.size(); ++i) {
            const std::string& argument = requestArguments[i];
            if (is_flag_argument(argument)) {
                if (argument == "--output") {
                    if (i + 1 >= requestArguments.size()) {
                        state.lastParseFailed = true;
                        return {};
                    }
                    packOutputPath = requestArguments[++i];
                    continue;
                }
                if (argument == "--payload-dir") {
                    if (i + 1 >= requestArguments.size()) {
                        state.lastParseFailed = true;
                        return {};
                    }
                    packPayloadPath = requestArguments[++i];
                    continue;
                }
                global_flags.push_back(argument.substr(2));
                continue;
            }
            positional.push_back(argument);
        }

        if (positional.size() > 2) {
            state.lastParseFailed = true;
            return {};
        }

        Request request;
        request.action = action;
        request.flags = global_flags;
        request.outputPath = packOutputPath;
        request.payloadPath = packPayloadPath;
        request.localPath = positional.empty() ? "." : positional.back();
        request.usesLocalTarget = true;

        if (positional.size() == 2) {
            const std::string system = to_lower_copy(positional.front());
            if (!discover_systems(config).contains(system)) {
                state.lastParseFailed = true;
                return {};
            }
            request.system = system;
        }

        requests.push_back(std::move(request));
        return requests;
    }

    if (action == ActionType::UPDATE) {
        bool hasNonFlagArgument = false;
        bool hasAllFlag = false;
        for (std::size_t i = actionIndex + 1; i < requestArguments.size(); ++i) {
            if (requestArguments[i] == "--all") {
                hasAllFlag = true;
            }
            if (!is_flag_argument(requestArguments[i])) {
                hasNonFlagArgument = true;
                break;
            }
        }
        if (!hasNonFlagArgument && !hasAllFlag) {
            return requests;
        }
    }

    const bool updateUsesSystemWidePackageMode = action == ActionType::UPDATE && update_command_has_package_mode_flag(arguments);

    auto consume_request_flag = [&](std::size_t& index, std::vector<std::string>& flags) -> bool {
        const std::string& argument = requestArguments[index];
        const std::size_t previousFlagCount = flags.size();
        if (consume_package_result_filter_flag(action, requestArguments, index, flags)) {
            if (flags.size() == previousFlagCount) {
                state.lastParseFailed = true;
                return false;
            }
            return true;
        }
        if (action == ActionType::SBOM && argument == "--format") {
            if (index + 1 >= requestArguments.size()) {
                state.lastParseFailed = true;
                return false;
            }
            sbomOutputFormat = requestArguments[++index];
            if (!sbom_output_format_from_string(sbomOutputFormat).has_value()) {
                state.lastParseFailed = true;
                return false;
            }
            return true;
        }
        if (action == ActionType::SBOM && argument == "--output") {
            if (index + 1 >= requestArguments.size()) {
                state.lastParseFailed = true;
                return false;
            }
            sbomOutputPath = requestArguments[++index];
            return true;
        }
        if (action == ActionType::AUDIT && argument == "--format") {
            if (index + 1 >= requestArguments.size()) {
                state.lastParseFailed = true;
                return false;
            }
            auditOutputFormat = requestArguments[++index];
            if (!audit_output_format_from_string(auditOutputFormat).has_value()) {
                state.lastParseFailed = true;
                return false;
            }
            return true;
        }
        if (action == ActionType::AUDIT && argument == "--output") {
            if (index + 1 >= requestArguments.size()) {
                state.lastParseFailed = true;
                return false;
            }
            auditOutputPath = requestArguments[++index];
            return true;
        }
        if (action == ActionType::SNAPSHOT && argument == "--output") {
            if (index + 1 >= requestArguments.size()) {
                state.lastParseFailed = true;
                return false;
            }
            snapshotOutputPath = requestArguments[++index];
            return true;
        }
        flags.push_back(argument.substr(2));
        return true;
    };

    auto assign_request_output_options = [&](Request& request) {
        if (action == ActionType::SBOM) {
            request.outputPath = sbomOutputPath;
            if (!sbomOutputFormat.empty()) {
                request.outputFormat = sbomOutputFormat;
            } else if (!sbomOutputPath.empty()) {
                request.outputFormat = to_string(SbomOutputFormat::CYCLONEDX_JSON);
            } else {
                request.outputFormat = to_string(config.sbom.defaultFormat);
            }
        }
        if (action == ActionType::AUDIT) {
            request.outputPath = auditOutputPath;
            if (!auditOutputFormat.empty()) {
                request.outputFormat = auditOutputFormat;
            } else if (!auditOutputPath.empty()) {
                const auto inferred = infer_audit_output_format_from_path(auditOutputPath);
                request.outputFormat = to_string(inferred.value_or(AuditOutputFormat::CYCLONEDX_VEX_JSON));
            } else {
                request.outputFormat = to_string(AuditOutputFormat::TABLE);
            }
        }
        if (action == ActionType::SNAPSHOT) {
            request.outputPath = snapshotOutputPath;
        }
    };

    if (supports_manifest_path(action)) {
        for (std::size_t i = actionIndex + 1; i < requestArguments.size(); ++i) {
            const std::string& arg = requestArguments[i];
            if (is_flag_argument(arg)) {
                if (!consume_request_flag(i, global_flags)) {
                    return {};
                }
                continue;
            }

            const std::optional<std::filesystem::path> manifestPath = resolve_manifest_path_argument(arg);
            if (!manifestPath.has_value()) {
                break;
            }

            if (!std::filesystem::exists(manifestPath.value())) {
                Logger::instance().diagnostic(manifest_missing_diagnostic(manifestPath.value()));
                state.lastParseFailed = true;
                return {};
            }

            std::vector<ManifestEntry> entries;
            try {
                entries = ManifestLoader::load(manifestPath.value());
            } catch (const std::exception& error) {
                Logger::instance().diagnostic(manifest_load_diagnostic(manifestPath.value(), error.what()));
                state.lastParseFailed = true;
                return {};
            }

            if (entries.empty()) {
                Logger::instance().diagnostic(manifest_empty_diagnostic(manifestPath.value()));
                state.lastParseFailed = true;
                return {};
            }

            for (std::size_t j = i + 1; j < requestArguments.size(); ++j) {
                if (is_flag_argument(requestArguments[j])) {
                    if (!consume_request_flag(j, global_flags)) {
                        return {};
                    }
                }
            }

            std::unordered_map<std::string, std::size_t> manifestIndex;
            for (const ManifestEntry& entry : entries) {
                const std::string normalized = to_lower_copy(entry.system);
                const auto [it, inserted] = manifestIndex.emplace(normalized, requests.size());
                if (inserted) {
                    requests.push_back(Request{.action = action, .system = normalized});
                }
                Request& request = requests[it->second];
                const std::string packageSpecifier = entry.version.empty() ? entry.name : (entry.name + "@" + entry.version);
                request.packages.push_back(packageSpecifier);
                for (const std::string& flag : entry.flags) {
                    request.flags.push_back(flag);
                }
            }

            for (Request& request : requests) {
                for (const std::string& flag : global_flags) {
                    request.flags.push_back(flag);
                }
                assign_request_output_options(request);
            }

            return requests;
        }
    }

    const std::set<std::string> knownSystems = discover_systems(config);
    std::string currentSystem;

    auto assign_local_target = [&](Request& request, const std::string& system, const std::string& path) -> bool {
        if (!request.packages.empty() || (request.usesLocalTarget && request.localPath != path)) {
            Logger::instance().diagnostic(mixed_local_and_package_diagnostic(system));
            return false;
        }
        request.localPath = path;
        request.usesLocalTarget = true;
        return true;
    };

    auto ensure_request = [&](const std::string& system) -> Request& {
        const std::string normalizedSystem = to_lower_copy(system);
        const auto [it, inserted] = request_index_by_system.emplace(normalizedSystem, requests.size());
        if (inserted) {
            requests.push_back(Request{.action = action, .system = normalizedSystem});
        }
        return requests[it->second];
    };

    for (std::size_t i = actionIndex + 1; i < requestArguments.size(); ++i) {
        const std::string& argument = requestArguments[i];
        const std::string normalizedArgument = to_lower_copy(argument);

        if (is_flag_argument(argument)) {
            if (!consume_request_flag(i, global_flags)) {
                return {};
            }
            continue;
        }

        if (currentSystem.empty() && knownSystems.contains(normalizedArgument)) {
            ensure_request(normalizedArgument);
            currentSystem = normalizedArgument;
            continue;
        }

        if (action == ActionType::SEARCH && currentSystem.empty()) {
            Logger::instance().diagnostic(unknown_search_system_diagnostic(argument));
            state.lastParseFailed = true;
            return {};
        }

        if (action == ActionType::INSTALL && is_url(argument)) {
            if (!currentSystem.empty()) {
                Request& request = ensure_request(currentSystem);
                if (!assign_local_target(request, currentSystem, argument)) {
                    state.lastParseFailed = true;
                    return {};
                }
            } else {
                Request request;
                request.action = action;
                request.localPath = argument;
                request.usesLocalTarget = true;
                requests.push_back(std::move(request));
            }
            continue;
        }

        if (action == ActionType::INSTALL && is_existing_regular_file(argument)) {
            if (!currentSystem.empty()) {
                Request& request = ensure_request(currentSystem);
                if (!assign_local_target(request, currentSystem, argument)) {
                    state.lastParseFailed = true;
                    return {};
                }
            } else {
                Request request;
                request.action = action;
                request.localPath = argument;
                request.usesLocalTarget = true;
                requests.push_back(std::move(request));
            }
            continue;
        }

        const std::optional<std::pair<std::string, std::string>> scopedPackage = split_scoped_package_argument(argument, knownSystems);
        if (scopedPackage.has_value()) {
            Request& request = ensure_request(scopedPackage->first);
            if (request.usesLocalTarget) {
                Logger::instance().diagnostic(mixed_local_and_package_diagnostic(request.system));
                state.lastParseFailed = true;
                return {};
            }
            request.packages.push_back(scopedPackage->second);
            currentSystem = request.system;
            continue;
        }

        if (currentSystem.empty()) {
            ensure_request(normalizedArgument);
            currentSystem = normalizedArgument;
            continue;
        }

        if (action != ActionType::SEARCH && !current_system_prefers_package_tokens(currentSystem, action) && knownSystems.contains(normalizedArgument)) {
            ensure_request(normalizedArgument);
            currentSystem = normalizedArgument;
            continue;
        }

        Request& request = ensure_request(currentSystem);
        if (action == ActionType::INSTALL && is_existing_path(argument)) {
            if (!assign_local_target(request, currentSystem, argument)) {
                state.lastParseFailed = true;
                return {};
            }
            continue;
        }
        if (request.usesLocalTarget) {
            Logger::instance().diagnostic(mixed_local_and_package_diagnostic(currentSystem));
            state.lastParseFailed = true;
            return {};
        }
        request.packages.push_back(argument);
    }

    if (action == ActionType::ENSURE && requests.empty()) {
        for (const std::string& system : discover_primary_systems(config)) {
            requests.push_back(Request{.action = action, .system = system});
        }
    }

    if ((action == ActionType::LIST || action == ActionType::OUTDATED) && requests.empty()) {
        for (const std::string& system : discover_primary_systems(config)) {
            requests.push_back(Request{.action = action, .system = system});
        }
    }

    for (Request& request : requests) {
        request.flags = global_flags;
        if (action == ActionType::UPDATE && updateUsesSystemWidePackageMode && !request.system.empty() && request.system != "sys" &&
            request.system != "rqp" && !request.usesLocalTarget && request.packages.empty() && !has_flag(request.flags, "all")) {
            request.flags.push_back("all");
        }
        assign_request_output_options(request);
    }

    if (action == ActionType::UPDATE && requests.empty() && has_flag(global_flags, "all")) {
        for (const std::string& plugin : discover_non_builtin_plugins(config)) {
            Request request{.action = action, .system = plugin, .flags = global_flags};
            request.flags.push_back("__reqpack-internal-plugin-refresh-all");
            requests.push_back(std::move(request));
        }
    }

    if (action == ActionType::SBOM && requests.empty()) {
        for (const std::string& system : discover_primary_systems(config)) {
            Request request;
            request.action = action;
            request.system = system;
            request.flags = global_flags;
            assign_request_output_options(request);
            requests.push_back(std::move(request));
        }
    }

    if (action == ActionType::AUDIT && requests.empty()) {
        for (const std::string& system : discover_primary_systems(config)) {
            Request request;
            request.action = action;
            request.system = system;
            request.outputPath = auditOutputPath;
            if (!auditOutputFormat.empty()) {
                request.outputFormat = auditOutputFormat;
            } else if (!auditOutputPath.empty()) {
                const auto inferred = infer_audit_output_format_from_path(auditOutputPath);
                request.outputFormat = to_string(inferred.value_or(AuditOutputFormat::CYCLONEDX_VEX_JSON));
            } else {
                request.outputFormat = to_string(AuditOutputFormat::TABLE);
            }
            request.flags = global_flags;
            requests.push_back(std::move(request));
        }
    }

    if (action == ActionType::SNAPSHOT && requests.empty()) {
        Request request;
        request.action = action;
        request.flags = global_flags;
        request.outputPath = snapshotOutputPath;
        requests.push_back(std::move(request));
    }

    return requests;
}

}  // namespace cli_internal
