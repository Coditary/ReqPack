#include "cli/cli.h"

const std::string PROGRAM_NAME = "ReqPack";
const std::string USAGE = "Usage: ReqPack <command> <system> [packages...] [flags...]";
// const std::string FOOTER = "Commands:\n  install    Installs packages";
const std::string HELP_DESCRIPTION = "Displays this help";

Cli::Cli() : app(std::make_unique<CLI::App>("ReqPack - Unified Package Manager Interface")) {
    app->name(PROGRAM_NAME);
    app->allow_extras(true);
    app->prefix_command(true);
    app->set_help_flag("-h,--help", HELP_DESCRIPTION);
    app->usage(USAGE);
    // app->footer(FOOTER);
}

void Cli::parse(int argc, char* argv[]) {
    command.clear();
    packages.clear();
    flags.clear();

    if (argc < 2) {
        return;
    }

    try {
        app->parse(argc, argv);
    } catch (const CLI::ParseError&) {
        return;
    }

    const std::vector<std::string> arguments = app->remaining();
    if (arguments.empty()) {
        return;
    }

    command = arguments.front();

    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const std::string& arg = arguments[i];
        if (arg.rfind("--", 0) == 0) {
            flags[arg.substr(2)] = true;
            continue;
        }

        packages.push_back(arg);
    }
}

void Cli::print_help() {
    std::cout << app->help();
}
