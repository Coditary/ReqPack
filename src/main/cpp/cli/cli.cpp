#include "cli/cli.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <utility>

#include "core/manifest_loader.h"
#include "output/logger.h"

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

bool is_existing_path(const std::string& value) {
	std::error_code error;
	return std::filesystem::exists(std::filesystem::path(value), error) && !error;
}

}  // namespace

Cli::Cli() : app(std::make_unique<CLI::App>(PROGRAM_NAME + " - Unified Package Manager Interface")) {
    app->name(PROGRAM_NAME);
    app->allow_extras(true);
    app->prefix_command(true);
    app->set_help_flag("-h,--help", HELP_DESCRIPTION);
    app->usage(USAGE);
}

bool Cli::handleHelp(int argc, char* argv[]) {
    bool hasHelp = false;
    ActionType helpAction = ActionType::UNKNOWN;

    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (is_help_flag(arg)) {
            hasHelp = true;
        } else if (helpAction == ActionType::UNKNOWN) {
            const ActionType candidate = parse_action(arg);
            if (candidate != ActionType::UNKNOWN) {
                helpAction = candidate;
            }
        }
    }

    if (!hasHelp) {
        return false;
    }

    pendingHelpAction_ = helpAction;
    print_help();
    return true;
}

std::vector<Request> Cli::parse(int argc, char* argv[]) {
    return this->parse(argc, argv, DEFAULT_REQPACK_CONFIG);
}

std::vector<Request> Cli::parse(int argc, char* argv[], const ReqPackConfig& config) {
    std::vector<Request> requests;
    std::vector<std::string> global_flags;
    std::unordered_map<std::string, std::size_t> request_index_by_system;
    std::string sbomOutputFormat;
    std::string sbomOutputPath;

    if (argc < 2) {
        return requests;
    }

    // Pre-scan for help flags BEFORE app->parse() to avoid CLI11 internal state issues
    bool hasHelpFlag = false;
    ActionType helpAction = ActionType::UNKNOWN;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (is_help_flag(arg)) {
            hasHelpFlag = true;
        } else if (helpAction == ActionType::UNKNOWN) {
            const ActionType candidate = parse_action(arg);
            if (candidate != ActionType::UNKNOWN) {
                helpAction = candidate;
            }
        }
    }
    if (hasHelpFlag) {
        pendingHelpAction_ = helpAction;
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

    // Manifest mode: reqpack install <dir-path>
    // If the first non-flag argument after the action looks like a filesystem path
    // and resolves to a directory, load reqpack.lua from it.
    if (action == ActionType::INSTALL) {
        for (std::size_t i = actionIndex + 1; i < requestArguments.size(); ++i) {
            const std::string& arg = requestArguments[i];
            if (is_flag(arg)) {
                continue;
            }

            // Only treat as a path if it starts with '.' or '/'.
            // This avoids shadowing system names like "dnf" or "npm".
            const bool looksLikePath = !arg.empty() && (arg[0] == '.' || arg[0] == '/');
            if (!looksLikePath) {
                break;  // first non-flag arg is not a path → normal mode
            }

            std::error_code fsError;
            const std::filesystem::path candidatePath =
                std::filesystem::absolute(std::filesystem::path(arg), fsError);

            if (fsError || !std::filesystem::is_directory(candidatePath, fsError) || fsError) {
                break;  // path doesn't resolve to a directory → normal mode
            }

            const std::filesystem::path manifestPath = candidatePath / MANIFEST_FILENAME;
            if (!std::filesystem::exists(manifestPath)) {
                Logger::instance().err(
                    "no " + MANIFEST_FILENAME + " found in '" + candidatePath.string() + "'"
                );
                return {};
            }

            std::vector<ManifestEntry> entries;
            try {
                entries = ManifestLoader::load(manifestPath);
            } catch (const std::exception& e) {
                Logger::instance().err(
                    "failed to load manifest '" + manifestPath.string() + "': " + e.what()
                );
                return {};
            }

            if (entries.empty()) {
                Logger::instance().err(
                    "manifest '" + manifestPath.string() + "' contains no packages"
                );
                return {};
            }

            // Collect global flags from remaining arguments after the path.
            for (std::size_t j = i + 1; j < requestArguments.size(); ++j) {
                if (is_flag(requestArguments[j])) {
                    global_flags.push_back(requestArguments[j].substr(2));
                }
            }

            // Build one Request per system, batching packages for the same system.
            std::unordered_map<std::string, std::size_t> manifest_index;
            for (const ManifestEntry& entry : entries) {
                const std::string normalized = to_lower(entry.system);
                const auto [it, inserted] = manifest_index.emplace(normalized, requests.size());
                if (inserted) {
                    requests.push_back(Request{.action = action, .system = normalized});
                }
                Request& req = requests[it->second];
                // Encode version as name@version when present — plugins can parse this.
                const std::string pkgSpec =
                    entry.version.empty() ? entry.name : (entry.name + "@" + entry.version);
                req.packages.push_back(pkgSpec);
                // Merge per-entry flags into global flags for this request.
                for (const std::string& f : entry.flags) {
                    req.flags.push_back(f);
                }
            }

            for (Request& req : requests) {
                for (const std::string& f : global_flags) {
                    req.flags.push_back(f);
                }
            }

            return requests;
        }
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
            if (action == ActionType::SBOM && argument == "--format") {
                if (i + 1 >= requestArguments.size()) {
                    return {};
                }
                sbomOutputFormat = requestArguments[++i];
                continue;
            }
            if (action == ActionType::SBOM && argument == "--output") {
                if (i + 1 >= requestArguments.size()) {
                    return {};
                }
                sbomOutputPath = requestArguments[++i];
                continue;
            }
            global_flags.push_back(argument.substr(2));
            continue;
        }

        const std::optional<std::pair<std::string, std::string>> scoped_package = split_scoped_package(argument, known_systems);
        if (scoped_package.has_value()) {
            Request& request = ensure_request(scoped_package->first);
			if (request.usesLocalTarget) {
				Logger::instance().err("install cannot mix local path and package names for system '" + request.system + "'");
				return {};
			}
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
		if (action == ActionType::INSTALL && is_existing_path(argument)) {
			if (!request.packages.empty() || (request.usesLocalTarget && request.localPath != argument)) {
				Logger::instance().err("install cannot mix local path and package names for system '" + current_system + "'");
				return {};
			}
			request.localPath = argument;
			request.usesLocalTarget = true;
			continue;
		}
		if (request.usesLocalTarget) {
			Logger::instance().err("install cannot mix local path and package names for system '" + current_system + "'");
			return {};
		}
        request.packages.push_back(argument);
    }

    if (action == ActionType::ENSURE && requests.empty()) {
        for (const std::string& system : discover_primary_systems(config)) {
            requests.push_back(Request{.action = action, .system = system});
        }
    }

    for (Request& request : requests) {
        request.flags = global_flags;
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
    }

    if (action == ActionType::SBOM && requests.empty()) {
        Request request;
        request.action = action;
        request.outputPath = sbomOutputPath;
        if (!sbomOutputFormat.empty()) {
            request.outputFormat = sbomOutputFormat;
        } else if (!sbomOutputPath.empty()) {
            request.outputFormat = to_string(SbomOutputFormat::CYCLONEDX_JSON);
        } else {
            request.outputFormat = to_string(config.sbom.defaultFormat);
        }
        requests.push_back(std::move(request));
    }

    return requests;
}

ReqPackConfigOverrides Cli::parseConfigOverrides(int argc, char* argv[]) const {
    return extract_cli_config_overrides(argc, argv);
}

void Cli::print_help() {
    if (pendingHelpAction_ != ActionType::UNKNOWN) {
        print_command_help(pendingHelpAction_);
        return;
    }
    std::cout <<
        PROGRAM_NAME + " - Unified Package Manager Interface\n"
        "\n" +
        USAGE + "\n"
        "\nOptions:\n"
        "  -h,--help               " + HELP_DESCRIPTION + "\n"
        "\nCommands:\n"
        "  install                 Installs requested packages\n"
        "  remove                  Removes requested packages\n"
        "  update                  Updates requested packages\n"
        "  search                  Searches for packages\n"
        "  list                    Lists packages for a system\n"
        "  info                    Shows package info for a system\n"
        "  ensure [systems...]     Ensures plugin requirements are installed\n"
        "  sbom                    Exports planned graph as table or JSON\n"
        "\nConfig:\n"
        "  --config <path>         Loads config from a custom Lua file\n"
        "  --config=<path>         Same as above\n"
        "  --registry <path>       Loads registry sources from a custom path\n"
        "  --registry=<path>       Same as above\n"
        "\nSBOM:\n"
        "  --format <name>         Uses table, json, or cyclonedx-json\n"
        "  --output <path>         Writes SBOM output to file\n"
        "  --force                 Overwrites existing output file without prompting\n"
        "\nRun 'ReqPack <command> -h' for command-specific help.\n";
    std::cout.flush();
}

void Cli::print_command_help(ActionType action) {
    std::string help;
    switch (action) {
        case ActionType::INSTALL:
            help =
                "Usage: ReqPack install <system> [<package>...] [options]\n"
                "       ReqPack install <system> <local-path> [options]\n"
                "       ReqPack install <system1>:<package> <system2>:<package> [options]\n"
                "       ReqPack install <dir-path> [options]\n"
                "\n"
                "Install packages for one or more package managers.\n"
                "When a directory path is given (e.g. '.', './myproject', '/abs/path'),\n"
                "ReqPack reads a reqpack.lua manifest from that directory and installs\n"
                "all packages declared in it.\n"
                "\n"
                "Arguments:\n"
                "  <system>                Package manager to use (e.g. apt, brew, npm)\n"
                "  <package>               One or more package names to install\n"
                "  <local-path>            Local path to install from (cannot mix with package names)\n"
                "  <system>:<package>      Scoped package for a specific system\n"
                "  <dir-path>              Directory containing a reqpack.lua manifest\n"
                "\n"
                "Manifest format (reqpack.lua):\n"
                "  return {\n"
                "    packages = {\n"
                "      { system = \"dnf\",  name = \"curl\" },\n"
                "      { system = \"npm\",  name = \"express\", version = \"4.18.0\" },\n"
                "    }\n"
                "  }\n"
                "\n"
                "Options:\n"
                "  -h,--help               Displays this help\n"
                "  --dry-run               Show planned actions without executing them\n"
                "  --snyk                  Run Snyk vulnerability scan before install\n"
                "  --owasp                 Run OWASP/OSV vulnerability scan before install\n"
                "  --prompt-on-unsafe      Prompt before installing vulnerable packages\n"
                "  --abort-on-unsafe       Abort if vulnerable packages are found\n"
                "  --severity-threshold    Minimum severity to flag (low/medium/high/critical)\n"
                "  --score-threshold       Minimum CVSS score to flag (0.0-10.0)\n"
                "  --fail-on-unresolved-version    Abort if version cannot be resolved\n"
                "  --prompt-on-unresolved-version  Prompt if version cannot be resolved\n"
                "  --stop-on-first-failure Stop after the first failing system\n"
                "  --non-interactive       Disable all prompts (use defaults)\n"
                "\n"
                "Examples:\n"
                "  ReqPack install apt curl git\n"
                "  ReqPack install npm express lodash brew jq\n"
                "  ReqPack install apt:curl npm:express\n"
                "  ReqPack install brew ./my-formula.rb\n"
                "  ReqPack install .\n"
                "  ReqPack install ./myproject\n"
                "  ReqPack install /absolute/path/to/project\n";
            break;
        case ActionType::REMOVE:
            help =
                "Usage: ReqPack remove <system> [<package>...] [options]\n"
                "       ReqPack remove <system1>:<package> <system2>:<package> [options]\n"
                "\n"
                "Remove packages from one or more package managers.\n"
                "\n"
                "Arguments:\n"
                "  <system>                Package manager to use (e.g. apt, brew, npm)\n"
                "  <package>               One or more package names to remove\n"
                "  <system>:<package>      Scoped package for a specific system\n"
                "\n"
                "Options:\n"
                "  -h,--help               Displays this help\n"
                "  --dry-run               Show planned actions without executing them\n"
                "  --stop-on-first-failure Stop after the first failing system\n"
                "  --non-interactive       Disable all prompts (use defaults)\n"
                "\n"
                "Examples:\n"
                "  ReqPack remove apt curl\n"
                "  ReqPack remove npm express brew jq\n"
                "  ReqPack remove apt:curl npm:lodash\n";
            break;
        case ActionType::UPDATE:
            help =
                "Usage: ReqPack update <system> [<package>...] [options]\n"
                "       ReqPack update <system1>:<package> <system2>:<package> [options]\n"
                "\n"
                "Update packages for one or more package managers.\n"
                "If no packages are specified, all packages for the system are updated.\n"
                "\n"
                "Arguments:\n"
                "  <system>                Package manager to use (e.g. apt, brew, npm)\n"
                "  <package>               One or more package names to update (optional)\n"
                "  <system>:<package>      Scoped package for a specific system\n"
                "\n"
                "Options:\n"
                "  -h,--help               Displays this help\n"
                "  --dry-run               Show planned actions without executing them\n"
                "  --snyk                  Run Snyk vulnerability scan after update\n"
                "  --owasp                 Run OWASP/OSV vulnerability scan after update\n"
                "  --prompt-on-unsafe      Prompt before applying vulnerable updates\n"
                "  --abort-on-unsafe       Abort if vulnerable packages are found\n"
                "  --severity-threshold    Minimum severity to flag (low/medium/high/critical)\n"
                "  --score-threshold       Minimum CVSS score to flag (0.0-10.0)\n"
                "  --stop-on-first-failure Stop after the first failing system\n"
                "  --non-interactive       Disable all prompts (use defaults)\n"
                "\n"
                "Examples:\n"
                "  ReqPack update apt\n"
                "  ReqPack update npm express brew\n"
                "  ReqPack update apt:curl npm:express\n";
            break;
        case ActionType::SEARCH:
            help =
                "Usage: ReqPack search <system> <query> [options]\n"
                "\n"
                "Search for packages in a package manager.\n"
                "\n"
                "Arguments:\n"
                "  <system>                Package manager to search in (e.g. apt, brew, npm)\n"
                "  <query>                 Search term or package name\n"
                "\n"
                "Options:\n"
                "  -h,--help               Displays this help\n"
                "  --non-interactive       Disable all prompts\n"
                "\n"
                "Examples:\n"
                "  ReqPack search apt curl\n"
                "  ReqPack search npm react\n"
                "  ReqPack search brew \"json tool\"\n";
            break;
        case ActionType::LIST:
            help =
                "Usage: ReqPack list <system> [options]\n"
                "\n"
                "List installed packages for a package manager.\n"
                "\n"
                "Arguments:\n"
                "  <system>                Package manager to list packages for (e.g. apt, brew, npm)\n"
                "\n"
                "Options:\n"
                "  -h,--help               Displays this help\n"
                "  --non-interactive       Disable all prompts\n"
                "\n"
                "Examples:\n"
                "  ReqPack list apt\n"
                "  ReqPack list npm\n"
                "  ReqPack list brew\n";
            break;
        case ActionType::INFO:
            help =
                "Usage: ReqPack info <system> <package> [options]\n"
                "\n"
                "Show detailed information about a package.\n"
                "\n"
                "Arguments:\n"
                "  <system>                Package manager to query (e.g. apt, brew, npm)\n"
                "  <package>               Package name to get info for\n"
                "\n"
                "Options:\n"
                "  -h,--help               Displays this help\n"
                "  --non-interactive       Disable all prompts\n"
                "\n"
                "Examples:\n"
                "  ReqPack info apt curl\n"
                "  ReqPack info npm express\n"
                "  ReqPack info brew jq\n";
            break;
        case ActionType::ENSURE:
            help =
                "Usage: ReqPack ensure [<system>...] [options]\n"
                "\n"
                "Ensure plugin requirements are installed for one or more systems.\n"
                "If no systems are specified, all known systems are checked.\n"
                "\n"
                "Arguments:\n"
                "  <system>                One or more package managers to check (optional)\n"
                "\n"
                "Options:\n"
                "  -h,--help               Displays this help\n"
                "  --dry-run               Show planned actions without executing them\n"
                "  --stop-on-first-failure Stop after the first failing system\n"
                "  --non-interactive       Disable all prompts (use defaults)\n"
                "\n"
                "Examples:\n"
                "  ReqPack ensure\n"
                "  ReqPack ensure apt brew\n";
            break;
        case ActionType::SBOM:
            help =
                "Usage: ReqPack sbom [<system>...] [options]\n"
                "\n"
                "Export a Software Bill of Materials (SBOM) for installed packages.\n"
                "If no systems are specified, all known systems are included.\n"
                "\n"
                "Arguments:\n"
                "  <system>                One or more package managers to include (optional)\n"
                "\n"
                "Options:\n"
                "  -h,--help               Displays this help\n"
                "  --format <name>         Output format: table, json, cyclonedx-json (default: table)\n"
                "  --output <path>         Write SBOM output to file instead of stdout\n"
                "  --force                 Overwrite existing output file without prompting\n"
                "  --non-interactive       Disable all prompts (use defaults)\n"
                "\n"
                "Examples:\n"
                "  ReqPack sbom\n"
                "  ReqPack sbom apt npm\n"
                "  ReqPack sbom --format json\n"
                "  ReqPack sbom --format cyclonedx-json --output sbom.json\n";
            break;
        default:
            print_help();
            return;
    }
    std::cout << help;
    std::cout.flush();
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
    if (normalized_command == "list") {
        return ActionType::LIST;
    }
    if (normalized_command == "info") {
        return ActionType::INFO;
    }
    if (normalized_command == "ensure") {
        return ActionType::ENSURE;
    }
    if (normalized_command == "sbom") {
        return ActionType::SBOM;
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

std::set<std::string> Cli::discover_primary_systems(const ReqPackConfig& config) {
    std::set<std::string> systems;
    const std::filesystem::path directory = config.registry.pluginDirectory;
    RegistryDatabase registryDatabase(config);

    if (registryDatabase.ensureReady()) {
        for (const RegistryRecord& record : registryDatabase.getAllRecords()) {
            if (!record.alias) {
                systems.insert(to_lower(record.name));
            }
        }
    }

    if (std::filesystem::exists(directory)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".lua") {
                continue;
            }

            if (entry.path().parent_path().filename() != entry.path().stem()) {
                continue;
            }

            systems.insert(to_lower(entry.path().stem().string()));
        }
    }

    for (const auto& [_, target] : config.planner.systemAliases) {
        systems.insert(to_lower(target));
    }

    return systems;
}

std::set<std::string> Cli::discover_systems(const ReqPackConfig& config) {
    std::set<std::string> systems;
    const std::filesystem::path directory = config.registry.pluginDirectory;
    RegistryDatabase registryDatabase(config);

    if (registryDatabase.ensureReady()) {
        for (const RegistryRecord& record : registryDatabase.getAllRecords()) {
            systems.insert(to_lower(record.name));
            if (record.alias && !record.source.empty()) {
                systems.insert(to_lower(record.source));
            }
        }
    }

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

        if (entry.path().parent_path().filename() != entry.path().stem()) {
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
