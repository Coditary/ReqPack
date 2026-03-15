#include "cli/cli.h"

int main(int argc, char* argv[]) {
    Cli cli;
    CliOutput args = cli.parse(argc, argv);

    if (args.command.empty()) {
        cli.print_help();
        return 0;
    }

    return 0;
}
