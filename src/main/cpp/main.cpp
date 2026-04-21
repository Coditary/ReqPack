#include "cli/cli.h"
#include "core/configuration.h"
#include "core/orchestrator.h"
#include "output/logger.h"
#include "plugins/lua_bridge.h"

int main(int argc, char* argv[]) {
    const ReqPackConfig config = DEFAULT_REQPACK_CONFIG;
    Logger logger;

    logger.setLevel(to_string(config.logging.level));
    logger.setPattern(config.logging.pattern);
    logger.setBacktrace(config.logging.enableBacktrace, config.logging.backtraceSize);
    if (config.logging.fileOutput) {
        logger.setFileSink(config.logging.filePath);
    }

    Cli cli;
    const std::vector<Request> requests = cli.parse(argc, argv);

    if (requests.empty()) {
        cli.print_help();
        return 0;
    }

    Orchestrator orchestrator(requests, config);
    orchestrator.run();

    return 0;
}
