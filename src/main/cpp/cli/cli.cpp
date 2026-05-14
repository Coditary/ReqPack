#include "cli/cli.h"

#include "cli_help_text.h"
#include "cli_parse_core.h"

#include <iostream>
#include <memory>
#include <vector>

Cli::Cli() : app(std::make_unique<CLI::App>(std::string(cli_internal::program_name()) + " - Unified Package Manager Interface")) {
    const std::string helpDescription(cli_internal::help_description());
    const std::string verboseDescription(cli_internal::verbose_description());

    app->name(std::string(cli_internal::program_name()));
    app->allow_extras(true);
    app->prefix_command(true);
    app->set_help_flag("-h,--help", helpDescription);
    app->add_flag("-v,--verbose", verboseDescription);
    app->usage(std::string(cli_internal::usage_text()));
}

bool Cli::handleHelp(int argc, char* argv[]) {
    pendingHelpAction_ = ActionType::UNKNOWN;
    lastParseFailed_ = false;

    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) {
        arguments.emplace_back(argv[i]);
    }

    const cli_internal::HelpScanResult helpScan = cli_internal::scan_help_arguments(arguments);
    if (!helpScan.hasHelp) {
        return false;
    }

    pendingHelpAction_ = helpScan.action;
    print_help();
    return true;
}

std::vector<Request> Cli::parse(int argc, char* argv[]) {
    return parse(argc, argv, default_reqpack_config());
}

std::vector<Request> Cli::parse(int argc, char* argv[], const ReqPackConfig& config) {
    return cli_internal::parse_argv(*app, argc, argv, config, cli_internal::CliParseState{
        .pendingHelpAction = pendingHelpAction_,
        .lastParseFailed = lastParseFailed_,
        .lastParseError = lastParseError_,
    });
}

std::vector<Request> Cli::parse(const std::vector<std::string>& arguments, const ReqPackConfig& config) {
    return cli_internal::parse_arguments(arguments, config, cli_internal::CliParseState{
        .pendingHelpAction = pendingHelpAction_,
        .lastParseFailed = lastParseFailed_,
        .lastParseError = lastParseError_,
    });
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
    std::cout << cli_internal::general_help_text();
    std::cout.flush();
}

void Cli::print_command_help(ActionType action) {
    const std::string help = cli_internal::command_help_text(action);
    if (help.empty()) {
        pendingHelpAction_ = ActionType::UNKNOWN;
        print_help();
        return;
    }
    std::cout << help;
    std::cout.flush();
}
