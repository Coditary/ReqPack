#include "plugins/lua_bridge_execution_policy.h"

#include "plugins/lua_bridge_value_mapper.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>

namespace {

struct ShellCommandInspection {
    bool requestsPrivilege{false};
    std::vector<std::string> writeTargets;
};

bool is_all_digits(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
    });
}

bool is_command_boundary_token(const std::string& token) {
    return token == "&&" || token == "||" || token == ";" || token == "|";
}

bool is_file_redirection_operator(const std::string& token) {
    return token.find('>') != std::string::npos && token.find('&') == std::string::npos;
}

bool is_option_token(const std::string& token) {
    return !token.empty() && token[0] == '-';
}

std::string command_basename(const std::string& token) {
    return LuaBridgeValueMapper::toLowerCopy(std::filesystem::path(token).filename().string());
}

std::vector<std::string> tokenize_shell_command(const std::string& command) {
    std::vector<std::string> tokens;
    std::string current;
    bool inSingleQuotes = false;
    bool inDoubleQuotes = false;
    bool escaping = false;

    const auto flush = [&]() {
        if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    };

    for (std::size_t index = 0; index < command.size(); ++index) {
        const char c = command[index];
        if (escaping) {
            current.push_back(c);
            escaping = false;
            continue;
        }

        if (inSingleQuotes) {
            if (c == '\'') {
                inSingleQuotes = false;
            } else {
                current.push_back(c);
            }
            continue;
        }

        if (inDoubleQuotes) {
            if (c == '"') {
                inDoubleQuotes = false;
            } else if (c == '\\' && index + 1 < command.size()) {
                current.push_back(command[++index]);
            } else {
                current.push_back(c);
            }
            continue;
        }

        if (c == '\\') {
            escaping = true;
            continue;
        }

        if (c == '\'') {
            inSingleQuotes = true;
            continue;
        }

        if (c == '"') {
            inDoubleQuotes = true;
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(c))) {
            flush();
            continue;
        }

        if ((c == '&' || c == '|') && index + 1 < command.size() && command[index + 1] == c) {
            flush();
            tokens.emplace_back(command.substr(index, 2));
            ++index;
            continue;
        }

        if (c == ';' || c == '|') {
            flush();
            tokens.emplace_back(1, c);
            continue;
        }

        if (c == '>') {
            std::string op;
            if (is_all_digits(current)) {
                op = current;
                current.clear();
            } else {
                flush();
            }
            op.push_back('>');
            if (index + 1 < command.size() && command[index + 1] == '>') {
                op.push_back('>');
                ++index;
            }
            if (index + 1 < command.size() && command[index + 1] == '&') {
                op.push_back('&');
                ++index;
            }
            tokens.push_back(op);
            continue;
        }

        current.push_back(c);
    }

    flush();
    return tokens;
}

void collect_write_targets_from_command_words(const std::vector<std::string>& words, ShellCommandInspection& inspection) {
    if (words.empty()) {
        return;
    }

    std::size_t commandIndex = 0;
    while (commandIndex < words.size() && words[commandIndex].find('=') != std::string::npos && words[commandIndex].find('/') == std::string::npos) {
        ++commandIndex;
    }
    if (commandIndex >= words.size()) {
        return;
    }

    std::string commandName = command_basename(words[commandIndex]);
    if (commandName == "env") {
        std::size_t nextIndex = commandIndex + 1;
        while (nextIndex < words.size() && (is_option_token(words[nextIndex]) ||
                                            (words[nextIndex].find('=') != std::string::npos && words[nextIndex].find('/') == std::string::npos))) {
            ++nextIndex;
        }
        if (nextIndex >= words.size()) {
            return;
        }
        commandIndex = nextIndex;
        commandName = command_basename(words[commandIndex]);
    }

    if (commandName == "sudo" || commandName == "doas") {
        inspection.requestsPrivilege = true;
        std::size_t nextIndex = commandIndex + 1;
        while (nextIndex < words.size() && is_option_token(words[nextIndex])) {
            ++nextIndex;
        }
        if (nextIndex >= words.size()) {
            return;
        }
        commandIndex = nextIndex;
        commandName = command_basename(words[commandIndex]);
    }

    if (commandName == "mkdir" || commandName == "touch" || commandName == "rm") {
        for (std::size_t index = commandIndex + 1; index < words.size(); ++index) {
            if (!is_option_token(words[index])) {
                inspection.writeTargets.push_back(words[index]);
            }
        }
        return;
    }

    if (commandName == "cp" || commandName == "mv") {
        std::string destination;
        for (std::size_t index = commandIndex + 1; index < words.size(); ++index) {
            if (!is_option_token(words[index])) {
                destination = words[index];
            }
        }
        if (!destination.empty()) {
            inspection.writeTargets.push_back(destination);
        }
    }
}

ShellCommandInspection inspect_shell_command(const std::string& command) {
    const std::vector<std::string> tokens = tokenize_shell_command(command);
    ShellCommandInspection inspection;

    std::size_t segmentStart = 0;
    while (segmentStart < tokens.size()) {
        while (segmentStart < tokens.size() && is_command_boundary_token(tokens[segmentStart])) {
            ++segmentStart;
        }
        if (segmentStart >= tokens.size()) {
            break;
        }

        std::size_t segmentEnd = segmentStart;
        while (segmentEnd < tokens.size() && !is_command_boundary_token(tokens[segmentEnd])) {
            ++segmentEnd;
        }

        std::vector<std::string> words;
        for (std::size_t index = segmentStart; index < segmentEnd; ++index) {
            if (is_file_redirection_operator(tokens[index])) {
                if (index + 1 < segmentEnd && !tokens[index + 1].empty()) {
                    inspection.writeTargets.push_back(tokens[index + 1]);
                }
                ++index;
                continue;
            }

            words.push_back(tokens[index]);
        }

        collect_write_targets_from_command_words(words, inspection);
        segmentStart = segmentEnd + 1;
    }

    return inspection;
}

std::filesystem::path current_working_directory_or(const std::filesystem::path& fallback) {
    std::error_code error;
    const std::filesystem::path current = std::filesystem::current_path(error);
    return error ? fallback : current;
}

std::filesystem::path normalize_exec_path(const std::string& rawPath, const std::filesystem::path& basePath) {
    const std::filesystem::path candidate(rawPath);
    if (candidate.empty()) {
        return {};
    }

    if (candidate.is_absolute()) {
        return candidate.lexically_normal();
    }
    return (basePath / candidate).lexically_normal();
}

bool path_has_prefix(const std::filesystem::path& path, const std::filesystem::path& prefix) {
    if (prefix.empty()) {
        return false;
    }

    auto pathIt = path.begin();
    auto prefixIt = prefix.begin();
    for (; prefixIt != prefix.end(); ++prefixIt, ++pathIt) {
        if (pathIt == path.end() || *pathIt != *prefixIt) {
            return false;
        }
    }
    return true;
}

bool is_safe_redirection_sink(const std::filesystem::path& path) {
    return path == std::filesystem::path("/dev/null") ||
           path == std::filesystem::path("/dev/stdout") ||
           path == std::filesystem::path("/dev/stderr");
}

bool write_scope_allows_path(const PluginWriteScope& scope,
                             const std::filesystem::path& targetPath,
                             const std::filesystem::path& pluginDirectory) {
    const std::string kind = LuaBridgeValueMapper::toLowerCopy(scope.kind);
    if (kind == "read-only") {
        return false;
    }

    if (kind == "temp") {
        return path_has_prefix(targetPath, std::filesystem::temp_directory_path().lexically_normal());
    }

    if (kind == "plugin-data") {
        std::filesystem::path base = pluginDirectory;
        if (!scope.value.empty()) {
            base /= scope.value;
        }
        return path_has_prefix(targetPath, base.lexically_normal());
    }

    if (kind == "reqpack-cache") {
        return path_has_prefix(targetPath, reqpack_cache_directory().lexically_normal());
    }

    if (kind == "user-home-subpath") {
        const char* home = std::getenv("HOME");
        if (home == nullptr || *home == '\0') {
            return false;
        }

        std::filesystem::path base(home);
        if (!scope.value.empty()) {
            base /= scope.value;
        }
        return path_has_prefix(targetPath, base.lexically_normal());
    }

    if (kind == "system-package-paths") {
        static const std::array<std::filesystem::path, 6> bases{
            std::filesystem::path("/usr"),
            std::filesystem::path("/usr/local"),
            std::filesystem::path("/opt"),
            std::filesystem::path("/etc"),
            std::filesystem::path("/var/lib"),
            std::filesystem::path("/var/cache"),
        };
        return std::any_of(bases.begin(), bases.end(), [&](const std::filesystem::path& base) {
            return path_has_prefix(targetPath, base);
        });
    }

    return false;
}

bool any_write_scope_allows_path(const std::vector<PluginWriteScope>& scopes,
                                 const std::filesystem::path& targetPath,
                                 const std::filesystem::path& pluginDirectory) {
    return std::any_of(scopes.begin(), scopes.end(), [&](const PluginWriteScope& scope) {
        return write_scope_allows_path(scope, targetPath, pluginDirectory);
    });
}

}  // namespace

std::optional<std::string> LuaBridgeExecutionPolicy::validate(const PluginSecurityMetadata& metadata,
                                                              const std::string& pluginId,
                                                              const std::string& pluginDirectory,
                                                              const std::string& command,
                                                              const std::vector<std::filesystem::path>& runtimeWriteRoots) {
    if (std::find(metadata.capabilities.begin(), metadata.capabilities.end(), "exec") == metadata.capabilities.end()) {
        return "execution policy denied for plugin '" + pluginId + "': shell command requires capability 'exec'.";
    }

    const ShellCommandInspection inspection = inspect_shell_command(command);
    if (inspection.requestsPrivilege && metadata.privilegeLevel != "sudo") {
        return "execution policy denied for plugin '" + pluginId + "': command requests privilege escalation but privilegeLevel is '" + metadata.privilegeLevel + "'.";
    }

    const std::filesystem::path workingDirectory = current_working_directory_or(std::filesystem::path(pluginDirectory));
    const std::filesystem::path normalizedPluginDirectory = std::filesystem::path(pluginDirectory).lexically_normal();
    for (const std::string& rawTarget : inspection.writeTargets) {
        const std::filesystem::path targetPath = normalize_exec_path(rawTarget, workingDirectory);
        if (targetPath.empty() || is_safe_redirection_sink(targetPath)) {
            continue;
        }

        if (std::any_of(runtimeWriteRoots.begin(), runtimeWriteRoots.end(), [&](const std::filesystem::path& allowedRoot) {
                return path_has_prefix(targetPath, allowedRoot.lexically_normal());
            })) {
            continue;
        }

        if (!any_write_scope_allows_path(metadata.writeScopes, targetPath, normalizedPluginDirectory)) {
            return "execution policy denied for plugin '" + pluginId + "': command writes outside declared writeScopes ('" + rawTarget + "').";
        }
    }

    return std::nullopt;
}
