#include "core/plugins/plugin_test_runner.h"

namespace {

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

}  // namespace

PluginTestCliParseResult parse_plugin_test_invocation(const std::vector<std::string>& arguments) {
    PluginTestCliParseResult result;

    std::size_t actionIndex = arguments.size();
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (arguments[i] == "test-plugin") {
            actionIndex = i;
            break;
        }
        ReqPackConfigOverrides ignoredOverrides;
        std::size_t configIndex = i;
        if (consume_cli_config_flag(arguments, configIndex, ignoredOverrides)) {
            i = configIndex;
            continue;
        }
        actionIndex = i;
        break;
    }

    if (actionIndex >= arguments.size() || arguments[actionIndex] != "test-plugin") {
        return result;
    }

    result.matched = true;

    for (std::size_t i = actionIndex + 1; i < arguments.size(); ++i) {
        const std::string& argument = arguments[i];
        if (argument == "-h" || argument == "--help") {
            result.helpRequested = true;
            continue;
        }
        if (argument == "--plugin" || starts_with(argument, "--plugin=")) {
            std::string value;
            if (argument == "--plugin") {
                if (i + 1 >= arguments.size()) {
                    result.error = "--plugin requires a value";
                    return result;
                }
                value = arguments[++i];
            } else {
                value = argument.substr(std::string{"--plugin="}.size());
            }
            result.invocation.plugin = value;
            continue;
        }
        if (argument == "--preset" || starts_with(argument, "--preset=")) {
            std::string value;
            if (argument == "--preset") {
                if (i + 1 >= arguments.size()) {
                    result.error = "--preset requires a value";
                    return result;
                }
                value = arguments[++i];
            } else {
                value = argument.substr(std::string{"--preset="}.size());
            }
            result.invocation.presets.push_back(value);
            continue;
        }
        if (argument == "--case" || starts_with(argument, "--case=")) {
            std::string value;
            if (argument == "--case") {
                if (i + 1 >= arguments.size()) {
                    result.error = "--case requires a value";
                    return result;
                }
                value = arguments[++i];
            } else {
                value = argument.substr(std::string{"--case="}.size());
            }
            result.invocation.caseFiles.push_back(value);
            continue;
        }
        if (argument == "--cases" || starts_with(argument, "--cases=")) {
            std::string value;
            if (argument == "--cases") {
                if (i + 1 >= arguments.size()) {
                    result.error = "--cases requires a value";
                    return result;
                }
                value = arguments[++i];
            } else {
                value = argument.substr(std::string{"--cases="}.size());
            }
            result.invocation.caseDirectories.push_back(value);
            continue;
        }
        if (argument == "--report" || starts_with(argument, "--report=")) {
            std::string value;
            if (argument == "--report") {
                if (i + 1 >= arguments.size()) {
                    result.error = "--report requires a value";
                    return result;
                }
                value = arguments[++i];
            } else {
                value = argument.substr(std::string{"--report="}.size());
            }
            result.invocation.reportPath = value;
            continue;
        }
        result.error = "unknown test-plugin argument: " + argument;
        return result;
    }

    if (!result.helpRequested) {
        if (result.invocation.plugin.empty()) {
            result.error = "test-plugin requires --plugin";
            return result;
        }
        if (result.invocation.presets.empty() && result.invocation.caseFiles.empty() && result.invocation.caseDirectories.empty()) {
            result.error = "test-plugin requires at least one --preset, --case, or --cases";
            return result;
        }
    }

    return result;
}

void print_plugin_test_help(std::ostream& output) {
    output
        << "rqp test-plugin - Run hermetic plugin conformance cases\n"
        << "\n"
        << "Usage:\n"
        << "  rqp test-plugin --plugin <path-or-id> --preset core [--report <file.json>]\n"
        << "  rqp test-plugin --plugin <path-or-id> --case <file.lua> [--case <file.lua> ...]\n"
        << "  rqp test-plugin --plugin <path-or-id> --cases <directory> [--report <file.json>]\n"
        << "\n"
        << "Options:\n"
        << "  --plugin <value>        Plugin script path, plugin directory, or plugin id\n"
        << "  --preset <name>         Adds built-in preset cases from <plugin>/.reqpack-test/<name>/\n"
        << "  --case <file.lua>       Adds one Lua test case file\n"
        << "  --cases <directory>     Adds all *.lua test case files from directory\n"
        << "  --report <file.json>    Writes JSON summary report\n"
        << "  -h,--help               Shows this help\n"
        << "\n"
        << "Known presets: core\n"
        << "Case file must return Lua table with request, fakeExec, and expect sections.\n";
}
