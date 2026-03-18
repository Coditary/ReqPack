#include "cli/cli.h"
#include "core/orchestrator.h"
#include "output/logger.h"

int main(int argc, char* argv[]) {
    Cli cli;
    CliOutput args = cli.parse(argc, argv);


    if (args.command.empty()) {
        cli.print_help();
        return 0;
    }

    return 0;
}
