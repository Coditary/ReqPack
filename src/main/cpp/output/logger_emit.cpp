#include "output/logger.h"
#include "output/logger_core.h"

namespace {

std::string join_pipe(const std::vector<std::string>& values) {
    std::string out;
    for (size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            out += '|';
        }
        out += values[index];
    }
    return out;
}

}  // namespace

std::uint64_t Logger::emit(OutputAction action, const OutputContext& context) {
    std::uint64_t eventId = 0;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        eventId = ++nextEventId;
        queue.push_back(OutputEvent{.id = eventId, .action = action, .context = context});
    }
    queueCondition.notify_one();
    return eventId;
}

std::uint64_t Logger::emitDiagnostic(const DiagnosticMessage& diagnostic, bool mirrorToDisplay) {
    return emit(OutputAction::DIAGNOSTIC,
        OutputContext{
            .level = diagnostic.severity,
            .message = diagnostic.summary,
            .category = diagnostic.category,
            .cause = diagnostic.cause,
            .recommendation = diagnostic.recommendation,
            .details = diagnostic.details,
            .source = diagnostic.source,
            .scope = diagnostic.scope,
            .contextFields = diagnostic.context,
            .mirrorToDisplay = mirrorToDisplay,
        });
}

void Logger::stdout(const std::string& message, const std::string& source, const std::string& scope) {
    emit(OutputAction::STDOUT,
        OutputContext{
            .level = spdlog::level::info,
            .message = message,
            .source = source,
            .scope = scope,
        });
}

void Logger::flush() {
    emit(OutputAction::FLUSH);
}

void Logger::flushSync() {
    const std::uint64_t flushEventId = emit(OutputAction::FLUSH);
    std::unique_lock<std::mutex> lock(processedMutex);
    processedCondition.wait(lock, [&]() {
        return processedEventId >= flushEventId;
    });
}

void Logger::crit(const std::string& message) {
    emit(OutputAction::LOG, OutputContext{.level = spdlog::level::critical, .message = message});
}

void Logger::err(const std::string& message) {
    emit(OutputAction::LOG, OutputContext{.level = spdlog::level::err, .message = message});
}

void Logger::warn(const std::string& message) {
    emit(OutputAction::LOG, OutputContext{.level = spdlog::level::warn, .message = message});
}

void Logger::info(const std::string& message) {
    emit(OutputAction::LOG, OutputContext{.level = spdlog::level::info, .message = message});
}

void Logger::debug(const std::string& message) {
    emit(OutputAction::LOG, OutputContext{.level = spdlog::level::debug, .message = message});
}

void Logger::trace(const std::string& message) {
    emit(OutputAction::LOG, OutputContext{.level = spdlog::level::trace, .message = message});
}

void Logger::diagnostic(const DiagnosticMessage& diagnostic, bool mirrorToDisplay) {
    (void)emitDiagnostic(diagnostic, mirrorToDisplay);
}

void Logger::displaySessionBegin(DisplayMode mode, const std::vector<std::string>& items) {
    emit(OutputAction::DISPLAY_SESSION_BEGIN,
        OutputContext{
            .payload = join_pipe(items),
            .displayMode = static_cast<int>(mode),
        });
}

void Logger::displaySessionEnd(bool success, int succeeded, int skipped, int failed) {
    emit(OutputAction::DISPLAY_SESSION_END,
        OutputContext{
            .statusCode = success ? 0 : 1,
            .payload = std::to_string(succeeded) + ":" + std::to_string(skipped) + ":" + std::to_string(failed),
        });
}

void Logger::displayItemBegin(const std::string& itemId, const std::string& label) {
    emit(OutputAction::DISPLAY_ITEM_BEGIN, OutputContext{.message = label, .source = itemId});
}

void Logger::displayItemProgress(const std::string& itemId, const DisplayProgressMetrics& rawMetrics) {
    const DisplayProgressMetrics metrics = canonicalize_progress_metrics(rawMetrics);
    emit(OutputAction::DISPLAY_ITEM_PROGRESS,
        OutputContext{
            .source = itemId,
            .progressPercent = metrics.percent,
            .currentBytes = metrics.currentBytes,
            .totalBytes = metrics.totalBytes,
            .bytesPerSecond = metrics.bytesPerSecond,
        });
}

void Logger::displayItemStep(const std::string& itemId, const std::string& step) {
    emit(OutputAction::DISPLAY_ITEM_STEP, OutputContext{.message = step, .source = itemId});
}

void Logger::displayItemSuccess(const std::string& itemId) {
    emit(OutputAction::DISPLAY_ITEM_SUCCESS, OutputContext{.source = itemId});
}

void Logger::displayItemFailure(const std::string& itemId, const std::string& reason) {
    emit(OutputAction::DISPLAY_ITEM_FAILURE, OutputContext{.message = reason, .source = itemId});
}

void Logger::displayItemFailure(const std::string& itemId, const DiagnosticMessage& diagnostic) {
    displayItemFailure(itemId, diagnostic.summary);
    displayDiagnostic(diagnostic, itemId, false);
}

void Logger::displayDiagnostic(const DiagnosticMessage& diagnostic, const std::string& sourceOverride, bool includeSummary) {
    const std::string message = logger_render_display_message(diagnostic, includeSummary);
    if (message.empty()) {
        return;
    }
    emit(OutputAction::DISPLAY_MESSAGE,
        OutputContext{
            .message = message,
            .source = sourceOverride.empty() ? diagnostic.source : sourceOverride,
        });
}

void Logger::displayTableHeader(const std::vector<std::string>& headers) {
    emit(OutputAction::DISPLAY_TABLE_HEADER, OutputContext{.payload = join_pipe(headers)});
}

void Logger::displayTableRow(const std::vector<std::string>& cells) {
    emit(OutputAction::DISPLAY_TABLE_ROW, OutputContext{.payload = join_pipe(cells)});
}

void Logger::displayTableEnd() {
    emit(OutputAction::DISPLAY_TABLE_END);
}
