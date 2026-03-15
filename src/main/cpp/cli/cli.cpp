#include "cli/cli.h"
#include <iostream>

Cli::Cli() {}

CliOutput Cli::parse(int argc, char* argv[]) {
    CliOutput args;

    if (argc < 2) {
        return args;
    }

    args.command = argv[1];

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg.substr(0, 2) == "--") {
            args.flags[arg.substr(2)] = true;
        } else {
            args.packages.push_back(arg);
        }
    }

    return args;
}

void Cli::print_help() {
    std::cout << "ReqPack - Unified Package Manager Interface\n"
              << "Usage: ReqPack <command> <system> [packages...] [flags...]\n\n"
              << "Commands:\n"
              << "  install    Installs packages\n";
}
