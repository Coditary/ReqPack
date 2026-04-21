#include "cli/cli.h"
#include "core/orchestrator.h"
#include "output/logger.h"
#include "plugins/lua_bridge.h"

int main(int argc, char* argv[]) {
    Cli cli;
    const std::vector<Request> requests = cli.parse(argc, argv);

    if (requests.empty()) {
        cli.print_help();
        return 0;
    }

    return 0;
}
