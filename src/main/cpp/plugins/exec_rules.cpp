#include "plugins/exec_rules.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <pty.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "plugins/exec_rules_core.h"

namespace {

struct LineAccumulator {
    std::string buffer;

    template <typename Callback>
    void append(const std::string& text, Callback&& callback) {
        std::size_t start = 0;
        while (start < text.size()) {
            const std::size_t newline = text.find('\n', start);
            if (newline == std::string::npos) {
                buffer.append(text.substr(start));
                break;
            }

            buffer.append(text.substr(start, newline - start));
            callback(buffer);
            buffer.clear();
            start = newline + 1;
        }
    }

    template <typename Callback>
    void flush(Callback&& callback) {
        if (buffer.empty()) {
            return;
        }
        const std::string line = buffer;
        buffer.clear();
        callback(line);
    }
};

std::string to_lower_copy(const std::string& value) {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return normalized;
}

void log_plugin_message(Logger& logger, spdlog::level::level_enum level, const std::string& pluginId, const std::string& message) {
    logger.emit(OutputAction::LOG, OutputContext{.level = level, .message = message, .source = "plugin", .scope = pluginId});
}

void log_rule_warning(Logger& logger, const std::string& pluginId, const std::string& message) {
    log_plugin_message(logger, spdlog::level::warn, pluginId, "exec-rule warning: " + message);
}

void log_rule_error(Logger& logger, const std::string& pluginId, const std::string& message) {
    log_plugin_message(logger, spdlog::level::err, pluginId, "exec-rule error: " + message);
}

OutputContext plugin_output_context(const std::string& sourceId, const std::string& pluginScope) {
    const bool hasItemId = sourceId.find(':') != std::string::npos;
    return OutputContext{.source = hasItemId ? sourceId : "plugin", .scope = pluginScope};
}

void log_exec_transcript_chunk(Logger& logger, const std::string& pluginId, const std::string& chunk, const bool mirrorToTerminal) {
    log_plugin_message(logger, spdlog::level::debug, pluginId, std::string("[exec] ") + chunk);
    if (mirrorToTerminal) {
        logger.stdout(chunk, pluginId, "exec");
    }
}

std::string escape_shell_double_quotes(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value) {
        if (c == '\\' || c == '"' || c == '$' || c == '`') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    return escaped;
}

std::optional<int> parse_int_value(const std::string& value) {
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(value, &consumed);
        if (consumed != value.size()) {
            return std::nullopt;
        }
        return parsed;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<double> parse_double_value(const std::string& value) {
    try {
        std::size_t consumed = 0;
        const double parsed = std::stod(value, &consumed);
        if (consumed != value.size()) {
            return std::nullopt;
        }
        return parsed;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

spdlog::level::level_enum parse_log_level(const std::string& value) {
    const std::string normalized = to_lower_copy(value);
    if (normalized.empty() || normalized == "info") {
        return spdlog::level::info;
    }
    if (normalized == "debug") {
        return spdlog::level::debug;
    }
    if (normalized == "warn") {
        return spdlog::level::warn;
    }
    if (normalized == "error") {
        return spdlog::level::err;
    }
    throw std::runtime_error("invalid log level '" + value + "'.");
}

bool write_all(int fd, const std::string& value) {
    std::size_t written = 0;
    while (written < value.size()) {
        const ssize_t count = ::write(fd, value.data() + written, value.size() - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    return true;
}

void emit_log_action(Logger& logger, const std::string& pluginId, spdlog::level::level_enum level, const std::string& message) {
    logger.emit(OutputAction::LOG, OutputContext{.level = level, .message = message, .source = "plugin", .scope = pluginId});
}

void emit_status_action(Logger& logger, const std::string& sourceId, const std::string& pluginScope, int statusCode) {
    OutputContext context = plugin_output_context(sourceId, pluginScope);
    context.statusCode = statusCode;
    logger.emit(OutputAction::PLUGIN_STATUS, context);
}

std::optional<std::uint64_t> parse_progress_bytes(const ResolvedExecRuleAction& action,
                                                 const std::string& valueKey,
                                                 const std::string& unitKey,
                                                 Logger& logger,
                                                 const std::string& pluginScope,
                                                 const std::string& label) {
    const auto valueIt = action.fields.find(valueKey);
    const auto unitIt = action.fields.find(unitKey);
    if (valueIt == action.fields.end() || unitIt == action.fields.end()) {
        return std::nullopt;
    }

    const std::optional<double> numeric = parse_double_value(valueIt->second);
    if (!numeric.has_value()) {
        log_rule_warning(logger, pluginScope, label + " value '" + valueIt->second + "' is not numeric.");
        return std::nullopt;
    }

    const std::optional<std::uint64_t> normalized = normalize_progress_units(numeric.value(), unitIt->second);
    if (!normalized.has_value()) {
        log_rule_warning(logger, pluginScope, label + " unit '" + unitIt->second + "' is invalid.");
        return std::nullopt;
    }
    return normalized;
}

std::optional<DisplayProgressMetrics> build_progress_metrics(Logger& logger,
                                                            const std::string& pluginScope,
                                                            const ResolvedExecRuleAction& action) {
    DisplayProgressMetrics metrics;
    if (const auto it = action.fields.find("percent"); it != action.fields.end()) {
        const std::optional<int> percent = parse_int_value(it->second);
        if (!percent.has_value()) {
            log_rule_warning(logger, pluginScope, "progress action value '" + it->second + "' is not an integer.");
        } else {
            metrics.percent = clamp_progress_percent(percent.value());
        }
    }

    metrics.currentBytes = parse_progress_bytes(action, "current", "currentUnit", logger, pluginScope, "progress current");
    metrics.totalBytes = parse_progress_bytes(action, "total", "totalUnit", logger, pluginScope, "progress total");
    metrics.bytesPerSecond = parse_progress_bytes(action, "speed", "speedUnit", logger, pluginScope, "progress speed");
    metrics = canonicalize_progress_metrics(metrics);

    if (!metrics.percent.has_value() && !metrics.currentBytes.has_value() && !metrics.totalBytes.has_value() && !metrics.bytesPerSecond.has_value()) {
        return std::nullopt;
    }
    return metrics;
}

void emit_progress_action(Logger& logger, const std::string& sourceId, const std::string& pluginScope, const DisplayProgressMetrics& metrics) {
    OutputContext context = plugin_output_context(sourceId, pluginScope);
    context.progressPercent = metrics.percent;
    context.currentBytes = metrics.currentBytes;
    context.totalBytes = metrics.totalBytes;
    context.bytesPerSecond = metrics.bytesPerSecond;
    logger.emit(OutputAction::PLUGIN_PROGRESS, context);
}

void emit_event_action(Logger& logger, const std::string& sourceId, const std::string& pluginScope, const std::string& name, const std::string& payload) {
    OutputContext context = plugin_output_context(sourceId, pluginScope);
    context.eventName = name;
    context.payload = payload;
    logger.emit(OutputAction::PLUGIN_EVENT, context);
}

void emit_artifact_action(Logger& logger, const std::string& sourceId, const std::string& pluginScope, const std::string& payload) {
    OutputContext context = plugin_output_context(sourceId, pluginScope);
    context.payload = payload;
    logger.emit(OutputAction::PLUGIN_ARTIFACT, context);
}

void dispatch_resolved_action(
    Logger& logger,
    const std::string& sourceId,
    const std::string& pluginScope,
    const ResolvedExecRuleAction& action,
    const std::optional<int>& masterFd
) {
    try {
        switch (action.type) {
            case ExecRuleActionType::Send: {
                if (!masterFd.has_value()) {
                    log_rule_warning(logger, pluginScope, "send action skipped because PTY writer is unavailable.");
                    return;
                }
                const auto it = action.fields.find("value");
                const std::string value = it == action.fields.end() ? std::string{} : it->second;
                if (value.empty()) {
                    log_rule_warning(logger, pluginScope, "send action resolved to empty value.");
                    return;
                }
                if (!write_all(masterFd.value(), value)) {
                    log_rule_warning(logger, pluginScope, "send action failed to write to child process.");
                }
                return;
            }
            case ExecRuleActionType::State: {
                const auto it = action.fields.find("value");
                if (it == action.fields.end() || it->second.empty()) {
                    log_rule_warning(logger, pluginScope, "state action resolved to empty value.");
                }
                return;
            }
            case ExecRuleActionType::Log: {
                const std::string level = action.fields.contains("level") ? action.fields.at("level") : "info";
                const std::string message = action.fields.contains("message") ? action.fields.at("message") : std::string{};
                emit_log_action(logger, pluginScope, parse_log_level(level), message);
                return;
            }
            case ExecRuleActionType::Status: {
                const std::string raw = action.fields.contains("code") ? action.fields.at("code") : std::string{};
                const std::optional<int> code = parse_int_value(raw);
                if (!code.has_value()) {
                    log_rule_warning(logger, pluginScope, "status action value '" + raw + "' is not an integer.");
                    return;
                }
                emit_status_action(logger, sourceId, pluginScope, code.value());
                return;
            }
            case ExecRuleActionType::Progress: {
                const std::optional<DisplayProgressMetrics> metrics = build_progress_metrics(logger, pluginScope, action);
                if (!metrics.has_value()) {
                    return;
                }
                emit_progress_action(logger, sourceId, pluginScope, metrics.value());
                return;
            }
            case ExecRuleActionType::BeginStep:
                emit_event_action(logger, sourceId, pluginScope, "begin_step", action.fields.contains("label") ? action.fields.at("label") : std::string{});
                return;
            case ExecRuleActionType::Success:
                emit_event_action(logger, sourceId, pluginScope, "success", "ok");
                return;
            case ExecRuleActionType::Failed:
                emit_event_action(logger, sourceId, pluginScope, "failed", action.fields.contains("message") ? action.fields.at("message") : std::string{});
                return;
            case ExecRuleActionType::Event: {
                const std::string name = action.fields.contains("name") ? action.fields.at("name") : std::string{};
                if (name.empty()) {
                    log_rule_warning(logger, pluginScope, "event action resolved to empty name.");
                    return;
                }
                emit_event_action(logger, sourceId, pluginScope, name, action.fields.contains("payload") ? action.fields.at("payload") : std::string{});
                return;
            }
            case ExecRuleActionType::Artifact:
                emit_artifact_action(logger, sourceId, pluginScope, action.fields.contains("payload") ? action.fields.at("payload") : std::string{});
                return;
        }
    } catch (const std::exception& error) {
        log_rule_warning(logger, pluginScope, error.what());
    }
}

void dispatch_evaluation_result(
    Logger& logger,
    const std::string& sourceId,
    const std::string& pluginScope,
    const ExecRuleEvaluationResult& result,
    const std::optional<int>& masterFd
) {
    for (const ResolvedExecRuleAction& action : result.actions) {
        dispatch_resolved_action(logger, sourceId, pluginScope, action, masterFd);
    }
}

bool read_fd_to_result(Logger& logger, const std::string& pluginId, int fd, ExecResult& result, const std::function<void(const std::string&)>& onChunk) {
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count > 0) {
            const std::string chunk(buffer.data(), static_cast<std::size_t>(count));
            result.stdoutText += chunk;
            onChunk(chunk);
            continue;
        }
        if (count == 0) {
            return true;
        }
        if (errno == EINTR) {
            continue;
        }
        result.stderrText = std::string("read failed: ") + std::strerror(errno);
        return false;
    }
}

ExecResult run_shell_command(Logger& logger, const std::string& pluginScope, const std::string& command, const std::function<void(const std::string&)>& onChunk, const bool silent) {
    ExecResult result;

    int pipeFds[2] = {-1, -1};
    if (::pipe(pipeFds) != 0) {
        result.stderrText = std::string("failed to create pipe: ") + std::strerror(errno);
        return result;
    }

    const pid_t child = fork();
    if (child < 0) {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        result.stderrText = std::string("failed to fork: ") + std::strerror(errno);
        return result;
    }

    if (child == 0) {
        ::close(pipeFds[0]);
        ::dup2(pipeFds[1], STDOUT_FILENO);
        ::dup2(pipeFds[1], STDERR_FILENO);
        ::close(pipeFds[1]);
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    ::close(pipeFds[1]);
    const auto consumeChunk = [&](const std::string& chunk) {
        log_exec_transcript_chunk(logger, pluginScope, chunk, !silent && logger.isConsoleOutputEnabled());
        onChunk(chunk);
    };
    (void)read_fd_to_result(logger, pluginScope, pipeFds[0], result, consumeChunk);
    ::close(pipeFds[0]);

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            result.stderrText = std::string("waitpid failed: ") + std::strerror(errno);
            result.exitCode = 1;
            result.success = false;
            return result;
        }
    }

    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : status;
    result.success = result.stderrText.empty() && result.exitCode == 0;
    if (!result.success && result.stderrText.empty()) {
        result.stderrText = result.stdoutText;
    }
    return result;
}

ExecResult run_plain_command(Logger& logger, const std::string& pluginScope, const std::string& command, const bool silent) {
    return run_shell_command(logger, pluginScope, command, [](const std::string&) {}, silent);
}

ExecResult run_line_command(Logger& logger, const std::string& sourceId, const std::string& pluginScope, const std::string& command, const ExecRuleset& ruleset, const bool silent) {
    ExecRuleRuntimeState runtime = make_exec_rule_runtime_state(ruleset);
    LineAccumulator lines;

    ExecResult result = run_shell_command(logger, pluginScope, command, [&](const std::string& chunk) {
        lines.append(chunk, [&](const std::string& line) {
            const ExecRuleEvaluationResult evaluation = evaluate_exec_rule_line_input(ruleset, runtime, line);
            dispatch_evaluation_result(logger, sourceId, pluginScope, evaluation, std::nullopt);
        });
    }, silent);
    lines.flush([&](const std::string& line) {
        const ExecRuleEvaluationResult evaluation = evaluate_exec_rule_line_input(ruleset, runtime, line);
        dispatch_evaluation_result(logger, sourceId, pluginScope, evaluation, std::nullopt);
    });
    return result;
}

ExecResult run_pty_command(Logger& logger, const std::string& sourceId, const std::string& pluginScope, const std::string& command, const ExecRuleset& ruleset, const bool silent) {
    ExecResult result;

    int masterFd = -1;
    const pid_t child = forkpty(&masterFd, nullptr, nullptr, nullptr);
    if (child < 0) {
        result.stderrText = std::string("failed to create PTY: ") + std::strerror(errno);
        return result;
    }

    if (child == 0) {
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    ExecRuleRuntimeState runtime = make_exec_rule_runtime_state(ruleset);
    LineAccumulator lines;
    std::string normalizedTranscript;

    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t count = ::read(masterFd, buffer.data(), buffer.size());
        if (count > 0) {
            const std::string chunk(buffer.data(), static_cast<std::size_t>(count));
            result.stdoutText += chunk;
            log_exec_transcript_chunk(logger, pluginScope, chunk, !silent && logger.isConsoleOutputEnabled());
            const std::string normalized = normalize_exec_rule_pty_chunk(chunk);
            if (!normalized.empty()) {
                normalizedTranscript += normalized;
                lines.append(normalized, [&](const std::string& line) {
                    const ExecRuleEvaluationResult evaluation = evaluate_exec_rule_line_input(ruleset, runtime, line);
                    dispatch_evaluation_result(logger, sourceId, pluginScope, evaluation, masterFd);
                });
                const ExecRuleEvaluationResult evaluation = evaluate_exec_rule_screen_input(ruleset, runtime, normalizedTranscript);
                dispatch_evaluation_result(logger, sourceId, pluginScope, evaluation, masterFd);
            }
            continue;
        }

        if (count == 0) {
            break;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EIO) {
            break;
        }

        result.stderrText = std::string("PTY read failed: ") + std::strerror(errno);
        break;
    }

    lines.flush([&](const std::string& line) {
        const ExecRuleEvaluationResult evaluation = evaluate_exec_rule_line_input(ruleset, runtime, line);
        dispatch_evaluation_result(logger, sourceId, pluginScope, evaluation, masterFd);
    });
    const ExecRuleEvaluationResult finalScreenEvaluation = evaluate_exec_rule_screen_input(ruleset, runtime, normalizedTranscript);
    dispatch_evaluation_result(logger, sourceId, pluginScope, finalScreenEvaluation, masterFd);

    ::close(masterFd);

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            result.stderrText = std::string("waitpid failed: ") + std::strerror(errno);
            result.exitCode = 1;
            result.success = false;
            return result;
        }
    }

    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : status;
    result.success = result.stderrText.empty() && result.exitCode == 0;
    return result;
}

}  // namespace

ExecResult run_plugin_command(Logger& logger, const std::string& sourceId, const std::string& pluginScope, const std::string& command, const bool silent) {
    return run_plain_command(logger, pluginScope, command, silent);
}

ExecResult run_plugin_command(Logger& logger, const std::string& sourceId, const std::string& pluginScope, const std::string& command, const sol::object& rules, const bool silent) {
    ExecRuleset ruleset;
    try {
        ruleset = parse_exec_rules(rules);
    } catch (const std::exception& error) {
        log_rule_error(logger, pluginScope, error.what());
        return ExecResult{.success = false, .exitCode = 1, .stdoutText = {}, .stderrText = error.what()};
    }

    switch (determine_exec_rule_runner_mode(ruleset)) {
        case ExecRuleRunnerMode::Plain:
            return run_plain_command(logger, pluginScope, command, silent);
        case ExecRuleRunnerMode::Line:
            return run_line_command(logger, sourceId, pluginScope, command, ruleset, silent);
        case ExecRuleRunnerMode::Pty:
            return run_pty_command(logger, sourceId, pluginScope, command, ruleset, silent);
    }

    return ExecResult{.success = false, .exitCode = 1, .stdoutText = {}, .stderrText = "unknown runner mode"};
}
