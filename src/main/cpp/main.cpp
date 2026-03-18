#include "cli/cli.h"
#include "core/orchestrator.h"
#include "output/logger.h"
#include "plugins/lua_bridge.h"

int main(int argc, char* argv[]) {
    Cli cli;
    CliOutput args = cli.parse(argc, argv);

	IPlugin* luaPlugin = new LuaBridge("plugins/sample_plugin.lua");

	luaPlugin->init();

	std::vector<Package> pkgs = { {"htop", "latest"}, {"vim", "9.0"} };

	luaPlugin->install(pkgs);

	luaPlugin->shutdown();


    if (args.command.empty()) {
        cli.print_help();
        return 0;
    }

    return 0;
}
