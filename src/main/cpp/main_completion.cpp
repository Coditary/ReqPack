#include "main_completion.h"

#include "cli/cli_command_completion.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::optional<std::string> parse_flag_value(const char* argument, std::string_view flag) {
    const std::string_view value(argument);
    if (value == flag) {
        return std::nullopt;
    }
    if (starts_with(value, flag) && value.size() > flag.size() && value[flag.size()] == '=') {
        return std::string(value.substr(flag.size() + 1));
    }
    return std::nullopt;
}

bool is_exact_flag(const char* argument, std::string_view flag) {
    return std::string_view(argument) == flag;
}

void print_completion_lines(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        std::cout << line << '\n';
    }
}

std::optional<std::filesystem::path> completions_source_directory() {
    if (const char* overridePath = std::getenv("RQPACK_COMPLETIONS_DIR"); overridePath != nullptr && *overridePath != '\0') {
        return std::filesystem::path(overridePath);
    }

#ifdef REQPACK_COMPLETIONS_SOURCE_DIR
    const std::filesystem::path configured(REQPACK_COMPLETIONS_SOURCE_DIR);
    if (std::filesystem::is_directory(configured)) {
        return configured;
    }
#endif

    return std::nullopt;
}

std::optional<std::string> read_completion_script(const std::filesystem::path& directory, const std::string& fileName) {
    const std::filesystem::path path = directory / fileName;
    std::ifstream input(path);
    if (!input.is_open()) {
        return std::nullopt;
    }

    std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return content;
}

int run_install_completion(const char* argv0) {
    const std::filesystem::path installScript =
#ifdef REQPACK_COMPLETIONS_SOURCE_DIR
        std::filesystem::path(REQPACK_COMPLETIONS_SOURCE_DIR).parent_path() / "install-rqp-completion.sh";
#else
        std::filesystem::path {};
#endif

    if (installScript.empty() || !std::filesystem::exists(installScript)) {
        if (argv0 != nullptr) {
            std::error_code errorCode;
            const auto executable = std::filesystem::weakly_canonical(argv0, errorCode);
            if (!errorCode) {
                const auto candidate = executable.parent_path().parent_path() / "scripts" / "install-rqp-completion.sh";
                if (std::filesystem::exists(candidate)) {
                    const std::string command = "bash \"" + candidate.string() + "\"";
                    return std::system(command.c_str());
                }
            }
        }
        std::cerr << "Error: install-rqp-completion.sh not found (set RQPACK_COMPLETIONS_DIR or build from source tree)\n";
        return 1;
    }

    const std::string command = "bash \"" + installScript.string() + "\"";
    return std::system(command.c_str());
}

}  // namespace

std::optional<int> try_handle_completion_cli(int argc, char* argv[]) {
    std::optional<std::string> completeSubcommandsPrefix;
    bool completeSubcommandsSeen = false;
    bool completeGlobalFlagsSeen = false;
    std::optional<std::string> completeGlobalFlagsPrefix;
    bool dumpCompletionSeen = false;
    std::string dumpCompletionShell;
    bool installCompletionSeen = false;

    for (int i = 1; i < argc; ++i) {
        const char* argument = argv[i];
        if (is_exact_flag(argument, "--complete-subcommands")) {
            completeSubcommandsSeen = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                completeSubcommandsPrefix = argv[++i];
            } else {
                completeSubcommandsPrefix = std::string {};
            }
            continue;
        }

        if (const std::optional<std::string> inlineValue = parse_flag_value(argument, "--complete-subcommands")) {
            completeSubcommandsSeen = true;
            completeSubcommandsPrefix = inlineValue;
            continue;
        }

        if (is_exact_flag(argument, "--complete-global-flags")) {
            completeGlobalFlagsSeen = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                completeGlobalFlagsPrefix = argv[++i];
            } else {
                completeGlobalFlagsPrefix = std::string {};
            }
            continue;
        }

        if (const std::optional<std::string> inlineGlobal = parse_flag_value(argument, "--complete-global-flags")) {
            completeGlobalFlagsSeen = true;
            completeGlobalFlagsPrefix = inlineGlobal;
            continue;
        }

        if (is_exact_flag(argument, "--dump-completion")) {
            dumpCompletionSeen = true;
            if (i + 1 < argc) {
                dumpCompletionShell = argv[++i];
            }
            continue;
        }

        if (const std::optional<std::string> inlineShell = parse_flag_value(argument, "--dump-completion")) {
            dumpCompletionSeen = true;
            dumpCompletionShell = inlineShell.value_or(std::string {});
            continue;
        }

        if (is_exact_flag(argument, "--install-completion")) {
            installCompletionSeen = true;
            continue;
        }
    }

    if (completeSubcommandsSeen) {
        print_completion_lines(cli_internal::list_subcommand_completions(completeSubcommandsPrefix.value_or(std::string {})));
        return 0;
    }

    if (completeGlobalFlagsSeen) {
        print_completion_lines(cli_internal::list_global_flag_completions(completeGlobalFlagsPrefix.value_or(std::string {})));
        return 0;
    }

    if (dumpCompletionSeen) {
        const std::optional<std::filesystem::path> sourceDir = completions_source_directory();
        if (!sourceDir.has_value()) {
            std::cerr << "Error: completion scripts directory not found (RQPACK_COMPLETIONS_DIR)\n";
            return 1;
        }

        const std::string fileName = dumpCompletionShell == "zsh" ? "rqp.zsh" : "rqp.bash";
        if (dumpCompletionShell != "bash" && dumpCompletionShell != "zsh") {
            std::cerr << "Error: unknown shell for --dump-completion: " << dumpCompletionShell << " (bash or zsh)\n";
            return 1;
        }

        const std::optional<std::string> script = read_completion_script(sourceDir.value(), fileName);
        if (!script.has_value()) {
            std::cerr << "Error: missing completion script: " << (sourceDir.value() / fileName).string() << '\n';
            return 1;
        }

        std::cout << script.value();
        return 0;
    }

    if (installCompletionSeen) {
        return run_install_completion(argc > 0 ? argv[0] : nullptr);
    }

    return std::nullopt;
}
