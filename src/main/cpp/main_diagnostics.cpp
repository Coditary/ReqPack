#include "main_diagnostics.h"

DiagnosticMessage config_override_diagnostic(const std::string& details) {
    return make_error_diagnostic(
        "config",
        "Invalid logging or runtime option",
        "One or more CLI configuration flags could not be parsed.",
        "Check flag spelling and required values, then run command again.",
        details,
        "cli",
        "config"
    );
}

DiagnosticMessage stdin_syntax_diagnostic(std::size_t lineNumber, const std::string& commandText) {
    return make_error_diagnostic(
        "cli",
        "stdin line " + std::to_string(lineNumber) + ": invalid command syntax",
        "Input line could not be tokenized because quotes or escapes are incomplete.",
        "Fix shell-style quoting on that line and try again.",
        commandText,
        "stdin",
        "batch",
        {{"line", std::to_string(lineNumber)}}
    );
}

DiagnosticMessage stdin_parse_diagnostic(std::size_t lineNumber, const std::string& commandText) {
    return make_error_diagnostic(
        "cli",
        "stdin line " + std::to_string(lineNumber) + ": command could not be parsed",
        "Parsed tokens did not form a valid rqp command.",
        "Check command structure or run same command directly with --help.",
        commandText,
        "stdin",
        "batch",
        {{"line", std::to_string(lineNumber)}}
    );
}

DiagnosticMessage stdin_action_only_diagnostic(std::size_t lineNumber, const std::string& action) {
    return make_error_diagnostic(
        "cli",
        "stdin line " + std::to_string(lineNumber) + ": only " + action + " commands are allowed here",
        "`rqp " + action + " --stdin` accepts only " + action + " subcommands.",
        "Use `rqp serve --stdin` for mixed commands, or keep stdin input to " + action + " commands only.",
        {},
        "stdin",
        "batch",
        {{"line", std::to_string(lineNumber)}}
    );
}

DiagnosticMessage stdin_empty_batch_diagnostic(const std::string& action) {
    return make_error_diagnostic(
        "cli",
        "stdin contained no " + action + " commands",
        "Only comments or empty lines were provided to " + action + " batch mode.",
        "Pipe at least one payload line or `" + action + " ...` command into rqp, or use normal CLI arguments.",
        {},
        "stdin",
        "batch"
    );
}

DiagnosticMessage self_update_diagnostic(const std::string& summary,
                                         const std::string& cause,
                                         const std::string& recommendation,
                                         const std::string& details) {
    return make_error_diagnostic(
        "self-update",
        summary,
        cause,
        recommendation,
        details,
        "self-update",
        "update"
    );
}

DiagnosticMessage plugin_test_diagnostic(const std::string& summary,
                                         const std::string& cause,
                                         const std::string& recommendation,
                                         const std::string& details) {
    return make_error_diagnostic(
        "plugin-test",
        summary,
        cause,
        recommendation,
        details,
        "test-plugin",
        "conformance"
    );
}
