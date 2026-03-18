#include "cli/cli.h"
#include "core/orchestrator.h"
#include "output/logger.h"
#include "plugins/lua_bridge.h"

int main(int argc, char* argv[]) {
    Cli cli;
    CliOutput args = cli.parse(argc, argv);

	Registry registry;
	registry.scanDirectory("./plugins");

	registry.loadPlugin("dnf");

	if (registry.isLoaded("dnf")) {
		IPlugin* luaPlugin = registry.getPlugin("dnf");
		luaPlugin->init();
		luaPlugin->search("btop");
		luaPlugin->shutdown();
	} else {
		Logger logger;
		logger.err("Plugin 'dnf' not found or failed to load.");
		for (const auto& name : registry.getAvailableNames()) {
			logger.info("Available plugin: " + name);
		}
	}


    if (args.command.empty()) {
        cli.print_help();
        return 0;
    }

    return 0;
}
