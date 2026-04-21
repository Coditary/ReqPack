#include "cli/cli.h"
#include "core/configuration.h"
#include "core/orchestrator.h"
#include "output/logger.h"
#include "plugins/lua_bridge.h"

#include <filesystem>

int main(int argc, char* argv[]) {
    Cli cli;
    const ReqPackConfigOverrides configOverrides = cli.parseConfigOverrides(argc, argv);
    const std::filesystem::path configPath = configOverrides.configPath.value_or(default_reqpack_config_path());
    const ReqPackConfig fileConfig = load_config_from_lua(configPath, DEFAULT_REQPACK_CONFIG);
    const ReqPackConfig config = apply_config_overrides(fileConfig, configOverrides);
    Logger logger;

    logger.setLevel(to_string(config.logging.level));
    logger.setPattern(config.logging.pattern);
    logger.setBacktrace(config.logging.enableBacktrace, config.logging.backtraceSize);
    if (config.logging.fileOutput) {
        logger.setFileSink(config.logging.filePath);
    }
    const std::vector<Request> requests = cli.parse(argc, argv);

    if (requests.empty()) {
        cli.print_help();
        return 0;
    }

    Orchestrator orchestrator(requests, config);
    orchestrator.run();

    return 0;
}
