#include "cli/cli.h"

#include "cli_help_text.h"
#include "cli_parse_core.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <vector>

namespace {

bool arguments_contain_help_all(const std::vector<std::string>& arguments) {
    return std::any_of(arguments.begin(), arguments.end(), [](const std::string& argument) {
        return argument == "--help-all";
    });
}

}  // namespace

Cli::Cli() : app(std::make_unique<CLI::App>(std::string(cli_internal::program_name()) + " - Unified Package Manager Interface")) {
    const std::string helpDescription(cli_internal::help_description());
    const std::string verboseDescription(cli_internal::verbose_description());

    app->name(std::string(cli_internal::program_name()));
    app->allow_extras(true);
    app->prefix_command(true);
    app->set_help_flag("-h,--help", helpDescription);
    app->add_flag("--help-all", "Show all advanced logging, security, and config options");
    app->add_flag("-V,--version", "Print ReqPack version");
    app->add_flag("-v,--verbose", verboseDescription);
    app->add_flag("-y,--yes", "Disable interactive prompts (alias: --non-interactive)");
    app->usage(std::string(cli_internal::usage_text()));
}

bool Cli::handleHelp(int argc, char* argv[]) {
    pendingHelpAction_ = ActionType::UNKNOWN;
    pendingHelpDedicatedCommand_.clear();
    lastParseFailed_ = false;

    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) {
        arguments.emplace_back(argv[i]);
    }

    if (arguments_contain_help_all(arguments)) {
        std::cout << cli_internal::extended_help_text();
        std::cout.flush();
        return true;
    }

    const cli_internal::HelpScanResult helpScan = cli_internal::scan_help_arguments(arguments);
    if (!helpScan.hasHelp) {
        return false;
    }

    pendingHelpAction_ = helpScan.action;
    pendingHelpDedicatedCommand_ = helpScan.dedicatedCommand;
    print_help();
    return true;
}

std::vector<Request> Cli::parse(int argc, char* argv[]) {
    return parse(argc, argv, default_reqpack_config());
}

std::vector<Request> Cli::parse(int argc, char* argv[], const ReqPackConfig& config) {
    return cli_internal::parse_argv(*app, argc, argv, config, cli_internal::CliParseState{
        .pendingHelpAction = pendingHelpAction_,
        .pendingHelpDedicatedCommand = pendingHelpDedicatedCommand_,
        .lastParseFailed = lastParseFailed_,
        .lastParseError = lastParseError_,
    });
}

std::vector<Request> Cli::parse(const std::vector<std::string>& arguments, const ReqPackConfig& config) {
    return cli_internal::parse_arguments(arguments, config, cli_internal::CliParseState{
        .pendingHelpAction = pendingHelpAction_,
        .pendingHelpDedicatedCommand = pendingHelpDedicatedCommand_,
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
    if (!pendingHelpDedicatedCommand_.empty()) {
        std::cout << cli_internal::dedicated_command_help_text(pendingHelpDedicatedCommand_);
        std::cout.flush();
        return;
    }
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
