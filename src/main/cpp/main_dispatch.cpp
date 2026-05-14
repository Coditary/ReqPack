#include "main_dispatch.h"

#include "main_arg_parsing.h"
#include "main_diagnostics.h"
#include "main_self_update.h"
#include "main_stdin.h"

#include "cli/cli.h"
#include "core/common/build_info.h"
#include "core/execution/orchestrator.h"
#include "core/host/host_info.h"
#include "core/plugins/plugin_test_runner.h"
#include "core/remote/remote_client.h"
#include "core/remote/remote_profiles.h"
#include "core/remote/serve_remote.h"
#include "output/command_output.h"
#include "output/idisplay.h"
#include "output/logger.h"

#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

int run_host_refresh(Logger& logger) {
    const std::shared_ptr<const HostInfoSnapshot> snapshot = HostInfoService::refreshSnapshot();
    logger.stdout("host refresh: cache updated");
    logger.stdout("host os: " + snapshot->platform.osFamily);
    logger.stdout("host arch: " + snapshot->platform.arch);
    logger.stdout("host cache: " + default_reqpack_host_info_cache_path().string());
    return 0;
}

int run_info_command(const ReqPackConfig& config, Logger& logger) {
    CommandOutput output;
    output.mode = DisplayMode::INFO;
    output.sessionItems = {"rqp"};
    output.blocks.push_back(make_command_field_value_block({
        {.key = "System", .value = "rqp"},
        {.key = "Name", .value = config.applicationName},
        {.key = "Version", .value = reqpack_build_release_id()},
        {.key = "Description", .value = "ReqPack self metadata"},
    }));
    output.success = true;
    output.succeeded = 1;
    render_command_output(output);
    logger.flushSync();
    return 0;
}

}  // namespace

int dispatch_main_command(Cli& cli,
                          const ReqPackConfig& config,
                          const std::filesystem::path& configPath,
                          const ReqPackConfigOverrides& configOverrides,
                          Logger& logger,
                          IDisplay* display,
                          const std::vector<std::string>& rawArguments,
                          int argc,
                          char* argv[]) {
    ServeRuntimeOptions serveOptions;
    std::string serveError;
    const bool isServeCommand = parse_serve_runtime_options(rawArguments, serveOptions, serveError);
    if (isServeCommand) {
        if (!serveError.empty()) {
            logger.diagnostic(make_error_diagnostic(
                "remote",
                "Serve command is invalid",
                "Remote or stdin serve options could not be parsed.",
                "Check serve flags and values, then run `rqp serve --help`.",
                serveError,
                "serve",
                "remote"
            ));
            logger.flushSync();
            return 1;
        }
        if (!serveOptions.readonlyExplicit) {
            serveOptions.readonly = config.remote.readonly;
        }
        if (!serveOptions.maxConnectionsExplicit) {
            serveOptions.maxConnections = config.remote.maxConnections;
        }
        if (serveOptions.stdin) {
            return run_stdin_serve_loop(cli, config, serveOptions.inheritedArguments);
        }
        return run_remote_serve(cli, config, configPath, configOverrides, logger, display, serveOptions);
    }

    RemoteClientInvocation remoteInvocation;
    std::string remoteError;
    const bool isRemoteCommand = parse_remote_client_invocation(rawArguments, remoteInvocation, remoteError);
    if (isRemoteCommand) {
        if (!remoteError.empty()) {
            logger.diagnostic(make_error_diagnostic(
                "remote",
                "Remote command is invalid",
                "Remote client invocation could not be parsed.",
                "Check remote profile name and forwarded command syntax, then run `rqp remote --help`.",
                remoteError,
                "remote",
                "client"
            ));
            logger.flushSync();
            return 1;
        }

        try {
            return run_remote_client(config,
                                     default_remote_profiles_path(),
                                     remoteInvocation.profileName,
                                     remoteInvocation.forwardedArguments,
                                     display);
        } catch (const std::exception& e) {
            logger.diagnostic(make_error_diagnostic(
                "remote",
                "Remote command execution failed",
                "ReqPack could not complete request against configured remote profile.",
                "Verify remote profile settings, connectivity, and authentication.",
                e.what(),
                remoteInvocation.profileName,
                "client"
            ));
            logger.flushSync();
            return 1;
        }
    }

    if (is_action_stdin_command(rawArguments, "install")) {
        return run_stdin_action_batch(cli, config, inherited_stream_arguments(rawArguments), ActionType::INSTALL, "install");
    }

    if (is_action_stdin_command(rawArguments, "remove")) {
        return run_stdin_action_batch(cli, config, inherited_stream_arguments(rawArguments), ActionType::REMOVE, "remove");
    }

    if (is_action_stdin_command(rawArguments, "update")) {
        return run_stdin_action_batch(cli, config, inherited_stream_arguments(rawArguments), ActionType::UPDATE, "update");
    }

    if (is_self_update_command(rawArguments)) {
        const int result = run_self_update(config, logger);
        logger.flushSync();
        return result;
    }

    if (is_host_refresh_command(rawArguments)) {
        const int result = run_host_refresh(logger);
        logger.flushSync();
        return result;
    }

    if (is_version_command(rawArguments)) {
        logger.stdout(config.applicationName + " " + reqpack_build_release_id());
        logger.flushSync();
        return 0;
    }

    if (is_info_command(rawArguments)) {
        return run_info_command(config, logger);
    }

    const PluginTestCliParseResult pluginTest = parse_plugin_test_invocation(rawArguments);
    if (pluginTest.matched) {
        if (pluginTest.helpRequested) {
            print_plugin_test_help(std::cout);
            logger.flushSync();
            return 0;
        }
        if (!pluginTest.error.empty()) {
            logger.diagnostic(plugin_test_diagnostic(
                "Plugin test invocation is invalid",
                "Required `test-plugin` arguments were missing or malformed.",
                "Run `rqp test-plugin --help` and retry with a plugin plus one or more case files.",
                pluginTest.error
            ));
            logger.flushSync();
            return 1;
        }

        try {
            const PluginTestRunReport report = run_plugin_test_cases(config, pluginTest.invocation);
            logger.flushSync();
            print_plugin_test_report(report, std::cout);
            logger.flushSync();
            return report.failed == 0 ? 0 : 1;
        } catch (const std::exception& error) {
            logger.diagnostic(plugin_test_diagnostic(
                "Plugin test execution failed",
                "ReqPack could not execute requested hermetic plugin test suite.",
                "Check plugin path, case files, and fake execution rules, then retry.",
                error.what()
            ));
            logger.flushSync();
            return 1;
        }
    }

    const std::vector<Request> requests = cli.parse(argc, argv, config);
    if (requests.empty()) {
        if (cli.parseFailed()) {
            if (!cli.lastParseError().empty()) {
                logger.diagnostic(config_override_diagnostic(cli.lastParseError()));
            }
            logger.flushSync();
            return 1;
        }
        cli.print_help();
        logger.flushSync();
        return 0;
    }

    Orchestrator orchestrator(requests, config);
    const int result = orchestrator.run();
    logger.flushSync();
    return result;
}
