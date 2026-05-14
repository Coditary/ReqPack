#include "main_dispatch.h"

#include "main_diagnostics.h"

#include "cli/cli.h"
#include "core/config/configuration.h"
#include "core/plugins/plugin_test_runner.h"
#include "output/display_factory.h"
#include "output/logger.h"

#include <curl/curl.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr const char* DEFAULT_MAIN_REGISTRY_URL = "https://github.com/Coditary/rqp-registry.git";

void configure_logger_from_config(Logger& logger, const ReqPackConfig& config) {
    logger.setLevel(to_string(config.logging.level));
    logger.setPattern(config.logging.pattern);
    logger.setBacktrace(config.logging.enableBacktrace, config.logging.backtraceSize);
    logger.setConsoleOutput(config.logging.consoleOutput);
    logger.setEnabledCategories(config.logging.enabledCategories);
    logger.setCaptureDisplayEvents(config.logging.captureDisplayEvents);
    if (config.logging.fileOutput) {
        logger.setFileSink(config.logging.filePath);
    } else {
        logger.disableFileSink();
    }
    if (config.logging.structuredFileOutput) {
        logger.setStructuredFileSink(config.logging.structuredFilePath);
    } else {
        logger.disableStructuredFileSink();
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    std::vector<std::string> earlyArguments;
    earlyArguments.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) {
        earlyArguments.emplace_back(argv[i]);
    }

    const PluginTestCliParseResult earlyPluginTest = parse_plugin_test_invocation(earlyArguments);
    if (earlyPluginTest.matched && earlyPluginTest.helpRequested) {
        print_plugin_test_help(std::cout);
        return 0;
    }

    {
        Cli earlyCliCheck;
        if (earlyCliCheck.handleHelp(argc, argv)) {
            return 0;
        }
    }

    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
        return 1;
    }

    Cli cli;
    const ReqPackConfigOverrides configOverrides = cli.parseConfigOverrides(argc, argv);
    if (configOverrides.errorMessage.has_value()) {
        Logger& logger = Logger::instance();
        logger.diagnostic(config_override_diagnostic(configOverrides.errorMessage.value()));
        logger.flushSync();
        curl_global_cleanup();
        return 1;
    }

    const std::filesystem::path configPath = configOverrides.configPath.value_or(default_reqpack_config_path());
    ReqPackConfig defaults = default_reqpack_config();
    defaults.registry.remoteUrl = DEFAULT_MAIN_REGISTRY_URL;
    const ReqPackConfig fileConfig = load_config_from_lua(configPath, defaults);
    ReqPackConfig config = apply_config_overrides(fileConfig, configOverrides);

    const std::filesystem::path workspacePluginDirectory = std::filesystem::current_path() / "plugins";
    if (!configOverrides.pluginDirectory.has_value() &&
        !configOverrides.configPath.has_value() &&
        config.registry.pluginDirectory == defaults.registry.pluginDirectory &&
        std::filesystem::exists(workspacePluginDirectory)) {
        config.registry.pluginDirectory = workspacePluginDirectory.string();
    }

    Logger& logger = Logger::instance();
    configure_logger_from_config(logger, config);

    std::unique_ptr<IDisplay> display = create_display(config.display);
    logger.setDisplay(display.get());

    std::vector<std::string> rawArguments;
    rawArguments.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) {
        rawArguments.emplace_back(argv[i]);
    }

    const int result = dispatch_main_command(
        cli,
        config,
        configPath,
        configOverrides,
        logger,
        display.get(),
        rawArguments,
        argc,
        argv
    );

    curl_global_cleanup();
    return result;
}
