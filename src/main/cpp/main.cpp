#include "cli/cli.h"
#include "core/orchestrator.h"
#include "output/logger.h"
#include "plugins/lua_bridge.h"

int main(int argc, char* argv[]) {
    Cli cli;
    cli.parse(argc, argv);

    if (cli.command.empty()) {
        cli.print_help();
        return 0;
    }

    return 0;
}
