#include "cli/cli.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <iostream>
#include <utility>

#include "core/manifest_loader.h"
#include "output/logger.h"

namespace {

const std::string PROGRAM_NAME = "ReqPack";
const std::string USAGE = "Usage: ReqPack <command> <system> [packages...] [additional systems/packages...] [flags...]";
const std::string HELP_DESCRIPTION = "Displays this help";
const std::string VERBOSE_DESCRIPTION = "Shows verbose command transcript and logger console output";

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

bool is_existing_regular_file(const std::string& value) {
    std::error_code error;
    return std::filesystem::is_regular_file(std::filesystem::path(value), error) && !error;
}

bool is_url(const std::string& value) {
    return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0 || value.rfind("file://", 0) == 0;
}

bool supports_manifest_path(ActionType action) {
    return action == ActionType::INSTALL || action == ActionType::AUDIT;
}

std::optional<std::filesystem::path> resolve_manifest_path_argument(const std::string& argument) {
    if (argument.empty()) {
        return std::nullopt;
    }

    const bool explicitPath = argument[0] == '.' || argument[0] == '/';
    const bool bareManifestFilename = std::filesystem::path(argument).filename() == MANIFEST_FILENAME;
    if (!explicitPath && !bareManifestFilename) {
        return std::nullopt;
    }

    std::error_code fsError;
    const std::filesystem::path candidatePath = std::filesystem::absolute(std::filesystem::path(argument), fsError);
    if (fsError) {
        return std::nullopt;
    }

    if (std::filesystem::is_directory(candidatePath, fsError) && !fsError) {
        return candidatePath / MANIFEST_FILENAME;
    }

    fsError.clear();
    if (std::filesystem::is_regular_file(candidatePath, fsError) && !fsError && candidatePath.filename() == MANIFEST_FILENAME) {
        return candidatePath;
    }

    return std::nullopt;
}

std::optional<AuditOutputFormat> infer_audit_output_format_from_path(const std::string& path) {
    const std::string extension = to_lower(std::filesystem::path(path).extension().string());
    if (extension == ".sarif") {
        return AuditOutputFormat::SARIF;
    }
    if (!extension.empty()) {
        return AuditOutputFormat::CYCLONEDX_VEX_JSON;
    }
	return std::nullopt;
}

bool consume_package_result_filter_flag(ActionType action,
	                                    const std::vector<std::string>& arguments,
	                                    std::size_t& index,
	                                    std::vector<std::string>& flags) {
	if (action != ActionType::SEARCH && action != ActionType::LIST && action != ActionType::OUTDATED) {
		return false;
	}
	const std::string& argument = arguments[index];
	const char* key = nullptr;
	if (argument == "--arch") {
		key = "arch";
	} else if (argument == "--type") {
		key = "type";
	} else {
		return false;
	}
	if (index + 1 >= arguments.size() || arguments[index + 1].empty()
	    || (arguments[index + 1].rfind("--", 0) == 0 && arguments[index + 1].size() > 2)) {
		return true;
	}
	flags.push_back(std::string(key) + "=" + to_lower(arguments[++index]));
	return true;
}

bool has_flag(const std::vector<std::string>& flags, const std::string& name) {
	return std::find(flags.begin(), flags.end(), name) != flags.end();
}

bool is_removed_security_backend_flag(const std::string& argument) {
    return argument == "--snyk" || argument == "--owasp";
}

bool current_system_prefers_package_tokens(const std::string& currentSystem, ActionType action) {
    if (to_lower(currentSystem) != "sys") {
        return false;
    }

    switch (action) {
        case ActionType::INSTALL:
        case ActionType::REMOVE:
        case ActionType::UPDATE:
        case ActionType::SEARCH:
        case ActionType::INFO:
        case ActionType::SBOM:
        case ActionType::AUDIT:
            return true;
        default:
            return false;
    }
}

}  // namespace

Cli::Cli() : app(std::make_unique<CLI::App>(PROGRAM_NAME + " - Unified Package Manager Interface")) {
    app->name(PROGRAM_NAME);
    app->allow_extras(true);
    app->prefix_command(true);
    app->set_help_flag("-h,--help", HELP_DESCRIPTION);
    app->add_flag("-v,--verbose", VERBOSE_DESCRIPTION);
    app->usage(USAGE);
}

bool Cli::handleHelp(int argc, char* argv[]) {
    pendingHelpAction_ = ActionType::UNKNOWN;
    lastParseFailed_ = false;
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
    return this->parse(argc, argv, default_reqpack_config());
}

std::vector<Request> Cli::parse(int argc, char* argv[], const ReqPackConfig& config) {
    pendingHelpAction_ = ActionType::UNKNOWN;
    lastParseFailed_ = false;
    lastParseError_.clear();
    if (argc < 2) {
        return {};
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
        return {};
    }

    try {
        app->parse(argc, argv);
    } catch (const CLI::ParseError&) {
        lastParseFailed_ = true;
        return {};
    }

    const std::vector<std::string> arguments = app->remaining();
    return parse(arguments, config);
}

std::vector<Request> Cli::parse(const std::vector<std::string>& arguments, const ReqPackConfig& config) {
    pendingHelpAction_ = ActionType::UNKNOWN;
    lastParseFailed_ = false;
    lastParseError_.clear();
    std::vector<Request> requests;
    std::vector<std::string> global_flags;
    std::unordered_map<std::string, std::size_t> request_index_by_system;
    std::string sbomOutputFormat;
    std::string sbomOutputPath;
    std::string auditOutputFormat;
    std::string auditOutputPath;
    std::string snapshotOutputPath;

    if (arguments.empty()) {
        return requests;
    }

    bool hasHelpFlag = false;
    ActionType helpAction = ActionType::UNKNOWN;
    for (const std::string& argument : arguments) {
        if (is_help_flag(argument)) {
            hasHelpFlag = true;
        } else if (helpAction == ActionType::UNKNOWN) {
            const ActionType candidate = parse_action(argument);
            if (candidate != ActionType::UNKNOWN) {
                helpAction = candidate;
            }
        }
    }
    if (hasHelpFlag) {
        pendingHelpAction_ = helpAction;
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
            if (ignoredOverrides.errorMessage.has_value()) {
                lastParseFailed_ = true;
                lastParseError_ = ignoredOverrides.errorMessage.value();
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

    if (!requestArguments.empty() && to_lower(requestArguments.front()) == to_lower(PROGRAM_NAME)) {
        requestArguments.erase(requestArguments.begin());
        if (requestArguments.empty()) {
            return requests;
        }
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
        lastParseFailed_ = true;
        return requests;
    }

    if (std::any_of(requestArguments.begin(), requestArguments.end(), [](const std::string& argument) {
            return is_removed_security_backend_flag(argument);
        })) {
        lastParseFailed_ = true;
        return {};
    }

    if (action == ActionType::REMOTE) {
        lastParseFailed_ = false;
        return requests;
    }

    if (action == ActionType::HOST) {
        lastParseFailed_ = false;
        return requests;
    }

	if (action == ActionType::UPDATE) {
		bool hasNonFlagArgument = false;
		bool hasAllFlag = false;
		for (std::size_t i = actionIndex + 1; i < requestArguments.size(); ++i) {
			if (requestArguments[i] == "--all") {
				hasAllFlag = true;
			}
			if (!is_flag(requestArguments[i])) {
				hasNonFlagArgument = true;
				break;
			}
		}
		if (!hasNonFlagArgument && !hasAllFlag) {
			lastParseFailed_ = false;
			return requests;
		}
	}

    // Manifest mode: reqpack install <dir-path>
    // If the first non-flag argument after the action looks like a filesystem path
    // and resolves to a directory, load reqpack.lua from it.
    if (supports_manifest_path(action)) {
        for (std::size_t i = actionIndex + 1; i < requestArguments.size(); ++i) {
            const std::string& arg = requestArguments[i];
            if (is_flag(arg)) {
                continue;
            }

            const std::optional<std::filesystem::path> manifestPath = resolve_manifest_path_argument(arg);
            if (!manifestPath.has_value()) {
                break;  // first non-flag arg is not a path → normal mode
            }

            if (!std::filesystem::exists(manifestPath.value())) {
                Logger::instance().err(
                    "no " + MANIFEST_FILENAME + " found in '" + manifestPath->parent_path().string() + "'"
                );
                lastParseFailed_ = true;
                return {};
            }

            std::vector<ManifestEntry> entries;
            try {
                entries = ManifestLoader::load(manifestPath.value());
            } catch (const std::exception& e) {
                Logger::instance().err(
                    "failed to load manifest '" + manifestPath->string() + "': " + e.what()
                );
                lastParseFailed_ = true;
                return {};
            }

            if (entries.empty()) {
                Logger::instance().err(
                    "manifest '" + manifestPath->string() + "' contains no packages"
                );
                lastParseFailed_ = true;
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

    auto assign_local_target = [&](Request& request, const std::string& system, const std::string& path) -> bool {
        if (!request.packages.empty() || (request.usesLocalTarget && request.localPath != path)) {
            Logger::instance().err("install cannot mix local path and package names for system '" + system + "'");
            return false;
        }
        request.localPath = path;
        request.usesLocalTarget = true;
        return true;
    };

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
			const std::size_t previousFlagCount = global_flags.size();
			if (consume_package_result_filter_flag(action, requestArguments, i, global_flags)) {
				if (global_flags.size() == previousFlagCount) {
					lastParseFailed_ = true;
					return {};
				}
				continue;
			}
			if (action == ActionType::SBOM && argument == "--format") {
                if (i + 1 >= requestArguments.size()) {
                    lastParseFailed_ = true;
                    return {};
                }
                sbomOutputFormat = requestArguments[++i];
                if (!sbom_output_format_from_string(sbomOutputFormat).has_value()) {
                    lastParseFailed_ = true;
                    return {};
                }
                continue;
            }
            if (action == ActionType::SBOM && argument == "--output") {
                if (i + 1 >= requestArguments.size()) {
                    lastParseFailed_ = true;
                    return {};
                }
                sbomOutputPath = requestArguments[++i];
                continue;
            }
            if (action == ActionType::AUDIT && argument == "--format") {
                if (i + 1 >= requestArguments.size()) {
                    lastParseFailed_ = true;
                    return {};
                }
                auditOutputFormat = requestArguments[++i];
                if (!audit_output_format_from_string(auditOutputFormat).has_value()) {
                    lastParseFailed_ = true;
                    return {};
                }
                continue;
            }
            if (action == ActionType::AUDIT && argument == "--output") {
                if (i + 1 >= requestArguments.size()) {
                    lastParseFailed_ = true;
                    return {};
                }
                auditOutputPath = requestArguments[++i];
                continue;
            }
            if (action == ActionType::SNAPSHOT && argument == "--output") {
                if (i + 1 >= requestArguments.size()) {
                    lastParseFailed_ = true;
                    return {};
                }
                snapshotOutputPath = requestArguments[++i];
                continue;
            }
            global_flags.push_back(argument.substr(2));
            continue;
        }

        // URL detection: handle before scoped-package and system-name checks.
        if (action == ActionType::INSTALL && is_url(argument)) {
            if (!current_system.empty()) {
                Request& request = ensure_request(current_system);
                if (!assign_local_target(request, current_system, argument)) {
					lastParseFailed_ = true;
                    return {};
                }
            } else {
                // No system known yet — system will be resolved from file extension at runtime.
                Request urlRequest;
                urlRequest.action = action;
                urlRequest.localPath = argument;
                urlRequest.usesLocalTarget = true;
                requests.push_back(std::move(urlRequest));
            }
            continue;
        }

        if (action == ActionType::INSTALL && is_existing_regular_file(argument)) {
            if (!current_system.empty()) {
                Request& request = ensure_request(current_system);
                if (!assign_local_target(request, current_system, argument)) {
					lastParseFailed_ = true;
                    return {};
                }
            } else {
                Request localRequest;
                localRequest.action = action;
                localRequest.localPath = argument;
                localRequest.usesLocalTarget = true;
                requests.push_back(std::move(localRequest));
            }
            continue;
        }

        const std::optional<std::pair<std::string, std::string>> scoped_package = split_scoped_package(argument, known_systems);
        if (scoped_package.has_value()) {            Request& request = ensure_request(scoped_package->first);
			if (request.usesLocalTarget) {
				Logger::instance().err("install cannot mix local path and package names for system '" + request.system + "'");
				lastParseFailed_ = true;
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

        if (!current_system_prefers_package_tokens(current_system, action) && known_systems.contains(normalized_argument)) {
            ensure_request(normalized_argument);
            current_system = normalized_argument;
            continue;
        }

        Request& request = ensure_request(current_system);
		if (action == ActionType::INSTALL && is_existing_path(argument)) {
			if (!assign_local_target(request, current_system, argument)) {
				lastParseFailed_ = true;
				return {};
			}
			continue;
		}
		if (request.usesLocalTarget) {
			Logger::instance().err("install cannot mix local path and package names for system '" + current_system + "'");
			lastParseFailed_ = true;
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
    }

	if (action == ActionType::UPDATE && requests.empty() && has_flag(global_flags, "all")) {
		for (const std::string& plugin : discover_non_builtin_plugins(config)) {
			Request request{.action = action, .system = plugin, .flags = global_flags};
			request.flags.push_back("__reqpack-internal-plugin-refresh-all");
			requests.push_back(std::move(request));
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
        request.outputPath = snapshotOutputPath;
        requests.push_back(std::move(request));
    }

    return requests;
}

ReqPackConfigOverrides Cli::parseConfigOverrides(int argc, char* argv[]) const {
    return extract_cli_config_overrides(argc, argv);
}

bool Cli::parseFailed() const {
    return lastParseFailed_;
}

const std::string& Cli::lastParseError() const {
    return lastParseError_;
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
        "  outdated                Shows outdated packages for a system\n"
        "  info                    Shows package info for a system\n"
        "  ensure [systems...]     Ensures plugin requirements are installed\n"
        "  sbom                    Exports planned graph as table or JSON\n"
        "  audit                   Audits planned graph for vulnerabilities\n"
        "  host                    Manages cached host system metadata\n"
        "  snapshot                Snapshots installed packages to reqpack.lua\n"
        "  serve                   Reads commands from stdin and keeps process running\n"
        "  remote                  Connects to remote profile from XDG config (~/.config/reqpack fallback)\n"
        "\nConfig:\n"
        "  --config <path>         Loads config from a custom Lua file\n"
        "  --config=<path>         Same as above\n"
        "  --registry <path>       Loads registry sources from a custom path\n"
        "  --registry=<path>       Same as above\n"
        "  --archive-password <value> Password for encrypted archives\n"
        "\nSBOM:\n"
        "  --format <name>         Uses table, json, or cyclonedx-json\n"
        "  --output <path>         Writes SBOM output to file\n"
        "\nAudit:\n"
        "  --format <name>         Uses table, json, cyclonedx-vex-json, or sarif\n"
        "  --output <path>         Writes audit output to file\n"
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
                "       ReqPack install --stdin [options]\n"
                "\n"
                "Install packages for one or more package managers.\n"
                "When a directory path is given (e.g. '.', './myproject', '/abs/path'),\n"
                "ReqPack reads a reqpack.lua manifest from that directory and installs\n"
                "all packages declared in it.\n"
                "When --stdin is given, ReqPack reads full install commands from stdin\n"
                "until EOF, then executes them as one combined install batch.\n"
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
                "  --stdin                 Read install commands from stdin until EOF\n"
                "  --dry-run               Show planned actions without executing them\n"
                "  --prompt-on-unsafe      Prompt before installing vulnerable packages\n"
                "  --abort-on-unsafe       Abort if vulnerable packages are found\n"
                "  --severity-threshold    Minimum severity to flag (low/medium/high/critical)\n"
                "  --score-threshold       Minimum CVSS score to flag (0.0-10.0)\n"
                "  --fail-on-unresolved-version    Abort if version cannot be resolved\n"
                "  --prompt-on-unresolved-version  Prompt if version cannot be resolved\n"
                "  --archive-password <value> Password for encrypted archives\n"
                "  --jobs <n>             Use exactly n workers for independent groups\n"
                "  --jobs-max             Use all logical CPU threads as workers\n"
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
                "  ReqPack install /absolute/path/to/project\n"
                "  printf 'install dnf curl\\ninstall npm express\\n' | ReqPack install --stdin\n";
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
                "Usage: ReqPack update [options]\n"
                "       ReqPack update <system> [<package>...] [options]\n"
                "       ReqPack update <system1>:<package> <system2>:<package> [options]\n"
                "\n"
                "Update ReqPack itself, plugin wrappers, or packages for one or more package managers.\n"
                "Without a system argument, ReqPack performs a self-update from its configured Git repository.\n"
                "With a system argument and no package list, ReqPack refreshes that plugin wrapper to its newest tagged version when the source is Git-backed.\n"
                "Use '--all' with a system to update all packages for that system instead.\n"
                "Use 'ReqPack update --all' to refresh all known plugin wrappers.\n"
                "To update a package-manager binary itself through ReqPack's wrapper layer, use 'ReqPack update sys <tool>'.\n"
                "\n"
                "Arguments:\n"
                "  <system>                Package manager to use (e.g. apt, brew, npm)\n"
                "  <package>               One or more package names to update (optional)\n"
                "  <system>:<package>      Scoped package for a specific system\n"
                "\n"
                "Options:\n"
                "  -h,--help               Displays this help\n"
                "  --dry-run               Show planned actions without executing them\n"
                "  --all                   Update all packages for system, or all plugin wrappers without a system\n"
                "  --prompt-on-unsafe      Prompt before applying vulnerable updates\n"
                "  --abort-on-unsafe       Abort if vulnerable packages are found\n"
                "  --severity-threshold    Minimum severity to flag (low/medium/high/critical)\n"
                "  --score-threshold       Minimum CVSS score to flag (0.0-10.0)\n"
                "  --stop-on-first-failure Stop after the first failing system\n"
                "  --non-interactive       Disable all prompts (use defaults)\n"
                "\n"
                "Examples:\n"
                "  ReqPack update\n"
                "  ReqPack update --all\n"
                "  ReqPack update pip\n"
                "  ReqPack update pip --all\n"
                "  ReqPack update apt\n"
                "  ReqPack update npm express brew\n"
                "  ReqPack update sys pip\n"
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
                "  --arch <value>          Filter search results by architecture (repeatable)\n"
                "  --type <value>          Filter search results by package type/class (repeatable)\n"
                "  --non-interactive       Disable all prompts\n"
                "\n"
                "Examples:\n"
                "  ReqPack search apt curl\n"
                "  ReqPack search npm react\n"
                "  ReqPack search brew \"json tool\"\n"
                "  ReqPack search dnf python3 --arch noarch --arch x86_64\n"
                "  ReqPack search dnf python3 --type doc --type devel\n";
            break;
        case ActionType::LIST:
            help =
                "Usage: ReqPack list [<system>] [options]\n"
                "\n"
                "List installed packages for a package manager.\n"
                "If no system is specified, all known primary systems are queried.\n"
                "\n"
                "Arguments:\n"
                "  <system>                Package manager to list packages for (optional)\n"
                "\n"
                "Options:\n"
                "  -h,--help               Displays this help\n"
                "  --arch <value>          Filter listed packages by architecture (repeatable)\n"
                "  --type <value>          Filter listed packages by package type/class (repeatable)\n"
                "  --non-interactive       Disable all prompts\n"
                "\n"
                "Examples:\n"
                "  ReqPack list\n"
                "  ReqPack list apt\n"
                "  ReqPack list npm\n"
                "  ReqPack list brew\n"
                "  ReqPack list dnf --arch x86_64\n"
                "  ReqPack list dnf --type doc --type devel\n";
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
                "  --wide                  Use wider table layout for terminal output\n"
                "  --no-wrap               Do not wrap table columns; let terminal handle overflow\n"
                "  --force                 Overwrite existing output file without prompting\n"
                "  --sbom-skip-missing-packages Skip requested packages that cannot be resolved\n"
                "  --non-interactive       Disable all prompts (use defaults)\n"
                "\n"
                "Examples:\n"
                "  ReqPack sbom\n"
                "  ReqPack sbom apt npm\n"
                "  ReqPack sbom --format json\n"
                "  ReqPack sbom --format cyclonedx-json --output sbom.json\n";
            break;
        case ActionType::AUDIT:
            help =
                "Usage: ReqPack audit [<system>...] [options]\n"
                "       ReqPack audit <system1>:<package> <system2>:<package> [options]\n"
                "       ReqPack audit <dir-path> [options]\n"
                "       ReqPack audit <manifest-path>/reqpack.lua [options]\n"
                "\n"
                "Audit planned packages for vulnerabilities and export findings.\n"
                "If no system is specified, all known primary systems are included.\n"
                "If a system is given without explicit packages, ReqPack audits installed packages reported by that system.\n"
                "Directory and direct reqpack.lua manifest input are supported.\n"
                "Without --output, audit prints a table and returns exit code 1 when findings exist.\n"
                "With --output, findings are exported but do not change the exit code.\n"
                "\n"
                "Options:\n"
                "  -h,--help               Displays this help\n"
                "  --format <name>         Output format: table, json, cyclonedx-vex-json, sarif\n"
                "  --output <path>         Write audit output to file instead of stdout\n"
                "  --wide                  Use wider table layout for terminal output\n"
                "  --no-wrap               Do not wrap table columns; let terminal handle overflow\n"
                "  --force                 Overwrite existing output file without prompting\n"
                "  --non-interactive       Disable all prompts (use defaults)\n"
                "\n"
                "Examples:\n"
                "  ReqPack audit\n"
                "  ReqPack audit npm react\n"
                "  ReqPack audit npm:react maven:org.junit:junit\n"
                "  ReqPack audit .\n"
                "  ReqPack audit ./reqpack.lua\n"
                "  ReqPack audit --format cyclonedx-vex-json --output audit.json\n"
                "  ReqPack audit --format sarif --output audit.sarif\n";
            break;
        case ActionType::OUTDATED:
            help =
                "Usage: ReqPack outdated [<system>] [options]\n"
                "\n"
                "Show packages that have newer versions available.\n"
                "If no system is specified, all known primary systems are queried.\n"
                "\n"
                "Arguments:\n"
                "  <system>                Package manager to check (optional)\n"
                "\n"
                "Options:\n"
                "  -h,--help               Displays this help\n"
                "  --arch <value>          Filter outdated packages by architecture (repeatable)\n"
                "  --type <value>          Filter outdated packages by package type/class (repeatable)\n"
                "  --non-interactive       Disable all prompts\n"
                "\n"
                "Examples:\n"
                "  ReqPack outdated\n"
                "  ReqPack outdated dnf\n"
                "  ReqPack outdated maven\n"
                "  ReqPack outdated dnf --arch noarch\n"
                "  ReqPack outdated dnf --type doc\n";
            break;
        case ActionType::HOST:
            help =
                "Usage: ReqPack host refresh\n"
                "\n"
                "Refresh the cached host system snapshot used by plugins via context.host and reqpack.host.\n"
                "This forces a live probe and rewrites the cache file under XDG cache.\n"
                "\n"
                "Subcommands:\n"
                "  refresh                 Force a live refresh of host metadata cache\n"
                "\n"
                "Examples:\n"
                "  ReqPack host refresh\n";
            break;
        case ActionType::SNAPSHOT:
            help =
                "Usage: ReqPack snapshot [options]\n"
                "\n"
                "Snapshot all currently installed packages (tracked by ReqPack history)\n"
                "into a reqpack.lua manifest. Use 'ReqPack install .' to restore on\n"
                "another machine.\n"
                "\n"
                "Options:\n"
                "  -h,--help               Displays this help\n"
                "  --output <path>         Write manifest to file instead of stdout\n"
                "  --force                 Overwrite existing file without prompting\n"
                "\n"
                "Examples:\n"
                "  ReqPack snapshot\n"
                "  ReqPack snapshot --output reqpack.lua\n"
                "  ReqPack snapshot --output reqpack.lua --force\n";
            break;
        case ActionType::SERVE:
            help =
                "Usage: ReqPack serve --stdin [options]\n"
                "       ReqPack serve --remote [--json|--http|--https] [options]\n"
                "\n"
                "Serve ReqPack commands either from stdin or over a remote socket.\n"
                "stdin mode executes one command per line until EOF.\n"
                "remote mode starts a TCP server and returns command responses to clients.\n"
                "Without protocol flag, remote mode accepts text protocol and auto-detects JSON requests per connection.\n"
                "Server-side users can be loaded from $XDG_CONFIG_HOME/reqpack/remote.lua (or ~/.config/reqpack/remote.lua) under users = { ... }.\n"
                "Admin users may run: shutdown, connections count, connections list, reload-config.\n"
                "--token/--username/--password remain fallback auth when no valid server users exist.\n"
                "\n"
                "Options:\n"
                "  -h,--help               Displays this help\n"
                "  --stdin                 Read commands from stdin\n"
                "  --remote                Start remote TCP command server\n"
                "  --json                  Force JSON Lines protocol for all remote clients\n"
                "  --http                  Reserved for future HTTP server mode\n"
                "  --https                 Reserved for future HTTPS server mode\n"
                "  --bind <addr>           Bind remote server to address (default: 127.0.0.1)\n"
                "  --port <n>              Bind remote server to port (default: 4545)\n"
                "  --token <value>         Require token authentication for remote clients\n"
                "  --username <name>       Require username/password authentication\n"
                "  --password <value>      Password for username/password authentication\n"
                "  --readonly              Allow read-only commands only\n"
                "  --max-connections <n>   Maximum concurrent remote clients\n"
                "  --non-interactive       Disable all prompts\n"
                "\n"
                "Examples:\n"
                "  printf 'install dnf curl\\nlist dnf\\n' | ReqPack serve --stdin\n"
                "  ReqPack serve --remote --bind 127.0.0.1 --port 4545 --token secret\n"
                "  ReqPack serve --remote --json --readonly --max-connections 4\n";
            break;
        case ActionType::REMOTE:
            help =
                "Usage: ReqPack remote <profile> [<command>...]\n"
                "\n"
                "Connect to remote profile from $XDG_CONFIG_HOME/reqpack/remote.lua or ~/.config/reqpack/remote.lua.\n"
                "With forwarded command, execute once and exit. Without command, start interactive text session.\n"
                "Forwarded install of local file uploads it to text/auto remotes: ReqPack remote <profile> install <system> <local-file>.\n"
                "JSON remotes reject file uploads.\n"
                "\n"
                "Arguments:\n"
                "  <profile>               Profile name defined in $XDG_CONFIG_HOME/reqpack/remote.lua\n"
                "  <command>               Optional forwarded ReqPack command\n"
                "\n"
                "Profile fields:\n"
                "  host/url                Remote host or IPv4 address\n"
                "  port                    Remote TCP port\n"
                "  protocol                auto, text, json, http, or https\n"
                "  token                   Token auth for text/json remotes\n"
                "  username/password       Basic auth for text/json remotes\n"
                "\n"
                "Examples:\n"
                "  ReqPack remote dev list apply\n"
                "  ReqPack remote prod\n";
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
    if (normalized_command == "audit") {
        return ActionType::AUDIT;
    }
    if (normalized_command == "outdated") {
        return ActionType::OUTDATED;
    }
    if (normalized_command == "host") {
        return ActionType::HOST;
    }
    if (normalized_command == "snapshot") {
        return ActionType::SNAPSHOT;
    }
    if (normalized_command == "serve") {
        return ActionType::SERVE;
    }
    if (normalized_command == "remote") {
        return ActionType::REMOTE;
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
    systems.insert("rqp");
    const std::filesystem::path directory = config.registry.pluginDirectory;
    RegistryDatabase registryDatabase(config);

    if (registryDatabase.ensureReady()) {
        for (const RegistryRecord& record : registryDatabase.getAllRecords()) {
            if (!record.alias) {
                systems.insert(to_lower(record.name));
            }
        }
    }

    for (const auto& [name, gateway] : config.security.gateways) {
        if (gateway.enabled) {
            systems.insert(to_lower(name));
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

std::set<std::string> Cli::discover_non_builtin_plugins(const ReqPackConfig& config) {
	std::set<std::string> systems;
	const std::filesystem::path directory = config.registry.pluginDirectory;
	const RegistrySourceMap configuredSources = collect_explicit_registry_sources(config);

	for (const auto& [name, entry] : configuredSources) {
		if (!entry.alias && !name.empty() && to_lower(name) != "rqp" && to_lower(name) != "sys") {
			systems.insert(to_lower(name));
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

			const std::string name = to_lower(entry.path().stem().string());
			if (name != "rqp" && name != "sys") {
				systems.insert(name);
			}
		}
	}

	return systems;
}

std::set<std::string> Cli::discover_systems(const ReqPackConfig& config) {
    std::set<std::string> systems;
    systems.insert("rqp");
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
        for (const auto& [name, gateway] : config.security.gateways) {
            if (gateway.enabled) {
                systems.insert(to_lower(name));
            }
        }
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

    for (const auto& [name, gateway] : config.security.gateways) {
        if (gateway.enabled) {
            systems.insert(to_lower(name));
        }
    }

    return systems;
}
