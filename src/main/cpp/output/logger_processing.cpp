#include "output/logger.h"
#include "output/logger_core.h"

#include <algorithm>
#include <iostream>
#include <sstream>

namespace {

std::vector<std::string> split_pipe(const std::string& value) {
    std::vector<std::string> out;
    std::istringstream stream(value);
    std::string token;
    while (std::getline(stream, token, '|')) {
        out.push_back(std::move(token));
    }
    if (!value.empty() && value.back() == '|') {
        out.emplace_back();
    }
    return out;
}

bool is_display_action(OutputAction action) {
    switch (action) {
        case OutputAction::DISPLAY_SESSION_BEGIN:
        case OutputAction::DISPLAY_SESSION_END:
        case OutputAction::DISPLAY_ITEM_BEGIN:
        case OutputAction::DISPLAY_ITEM_PROGRESS:
        case OutputAction::DISPLAY_ITEM_STEP:
        case OutputAction::DISPLAY_ITEM_SUCCESS:
        case OutputAction::DISPLAY_ITEM_FAILURE:
        case OutputAction::DISPLAY_MESSAGE:
        case OutputAction::DISPLAY_TABLE_HEADER:
        case OutputAction::DISPLAY_TABLE_ROW:
        case OutputAction::DISPLAY_TABLE_END:
            return true;
        default:
            return false;
    }
}

void emit_message_lines(IDisplay* display, const std::string& text, const std::string& source) {
    if (display == nullptr) {
        return;
    }
    std::istringstream stream(text);
    std::string line;
    bool emitted = false;
    while (std::getline(stream, line)) {
        display->onMessage(line, source);
        emitted = true;
    }
    if (!emitted && !text.empty()) {
        display->onMessage(text, source);
    }
}

bool is_item_scoped_plugin_source(const std::string& source) {
    return source != "plugin" && source.find(':') != std::string::npos;
}

std::string plugin_display_source(const OutputContext& context) {
    if (is_item_scoped_plugin_source(context.source)) {
        return context.source;
    }
    if (!context.scope.empty()) {
        return context.scope;
    }
    return context.source;
}

DisplayProgressMetrics progress_metrics_from_context(const OutputContext& context) {
    return canonicalize_progress_metrics(DisplayProgressMetrics{
        .percent = context.progressPercent,
        .currentBytes = context.currentBytes,
        .totalBytes = context.totalBytes,
        .bytesPerSecond = context.bytesPerSecond,
    });
}

}  // namespace

void Logger::routeToDisplay(const OutputEvent& event) {
    IDisplay* activeDisplay = display.load(std::memory_order_acquire);
    if (activeDisplay == nullptr) {
        return;
    }

    const OutputContext& context = event.context;
    if (event.action == OutputAction::DIAGNOSTIC) {
        emit_message_lines(activeDisplay, logger_diagnostic_text(context, false), context.source);
        return;
    }

    const bool itemScoped = is_item_scoped_plugin_source(context.source);
    const std::string displaySource = plugin_display_source(context);

    switch (event.action) {
        case OutputAction::PLUGIN_PROGRESS: {
            const DisplayProgressMetrics metrics = progress_metrics_from_context(context);
            if (itemScoped) {
                activeDisplay->onItemProgress(context.source, metrics);
            } else {
                const std::string summary = format_progress_summary(metrics);
                activeDisplay->onMessage(summary.empty() ? "progress" : "progress " + summary, displaySource);
            }
            break;
        }
        case OutputAction::PLUGIN_STATUS:
            activeDisplay->onMessage("status " + std::to_string(context.statusCode), displaySource);
            break;
        case OutputAction::PLUGIN_EVENT:
            if (context.eventName == "begin_step") {
                if (itemScoped) {
                    activeDisplay->onItemStep(context.source, context.payload);
                } else {
                    activeDisplay->onMessage(context.payload, displaySource);
                }
                break;
            }
            if (context.eventName == "success") {
                if (itemScoped) {
                    activeDisplay->onItemSuccess(context.source);
                } else {
                    activeDisplay->onMessage("done", displaySource);
                }
                break;
            }
            if (context.eventName == "failed") {
                if (itemScoped) {
                    activeDisplay->onItemFailure(context.source, context.payload);
                } else {
                    activeDisplay->onMessage(context.payload.empty() ? "failed" : "failed: " + context.payload, displaySource);
                }
                break;
            }
            if (context.eventName == "installed" || context.eventName == "deleted" || context.eventName == "updated"
                || context.eventName == "listed" || context.eventName == "searched" || context.eventName == "informed"
                || context.eventName == "outdated") {
                break;
            }
            activeDisplay->onMessage(context.eventName + ": " + context.payload, displaySource);
            break;
        case OutputAction::PLUGIN_ARTIFACT:
            activeDisplay->onMessage("artifact: " + context.payload, displaySource);
            break;
        case OutputAction::DISPLAY_SESSION_BEGIN:
            activeDisplay->onSessionBegin(static_cast<DisplayMode>(context.displayMode), split_pipe(context.payload));
            break;
        case OutputAction::DISPLAY_SESSION_END: {
            const bool ok = context.statusCode == 0;
            int succeeded = 0;
            int skipped = 0;
            int failed = 0;
            const auto firstSeparator = context.payload.find(':');
            const auto secondSeparator = firstSeparator == std::string::npos
                ? std::string::npos
                : context.payload.find(':', firstSeparator + 1);
            if (firstSeparator != std::string::npos && secondSeparator != std::string::npos) {
                succeeded = std::stoi(context.payload.substr(0, firstSeparator));
                skipped = std::stoi(context.payload.substr(firstSeparator + 1, secondSeparator - firstSeparator - 1));
                failed = std::stoi(context.payload.substr(secondSeparator + 1));
            }
            activeDisplay->onSessionEnd(ok, succeeded, skipped, failed);
            break;
        }
        case OutputAction::DISPLAY_ITEM_BEGIN:
            activeDisplay->onItemBegin(context.source, context.message.empty() ? context.source : context.message);
            break;
        case OutputAction::DISPLAY_ITEM_PROGRESS:
            activeDisplay->onItemProgress(context.source, progress_metrics_from_context(context));
            break;
        case OutputAction::DISPLAY_ITEM_STEP:
            activeDisplay->onItemStep(context.source, context.message);
            break;
        case OutputAction::DISPLAY_ITEM_SUCCESS:
            activeDisplay->onItemSuccess(context.source);
            break;
        case OutputAction::DISPLAY_ITEM_FAILURE:
            activeDisplay->onItemFailure(context.source, context.message);
            break;
        case OutputAction::DISPLAY_MESSAGE:
            emit_message_lines(activeDisplay, context.message, context.source);
            break;
        case OutputAction::DISPLAY_TABLE_HEADER:
            activeDisplay->onTableBegin(split_pipe(context.payload));
            break;
        case OutputAction::DISPLAY_TABLE_ROW:
            activeDisplay->onTableRow(split_pipe(context.payload));
            break;
        case OutputAction::DISPLAY_TABLE_END:
            activeDisplay->onTableEnd();
            break;
        case OutputAction::STDOUT:
            activeDisplay->onMessage(logger_format_message(context));
            break;
        default:
            break;
    }
}

void Logger::processEvent(const OutputEvent& event) {
    IDisplay* activeDisplay = display.load(std::memory_order_acquire);
    const bool categoryEnabled = isCategoryEnabled(event);
    const bool captureDisplay = shouldCaptureDisplayEvents();
    const bool persistStructured = categoryEnabled && (!is_display_action(event.action) || captureDisplay);
    if (persistStructured) {
        writeStructuredEvent(event);
    }

    switch (event.action) {
        case OutputAction::LOG:
            if (categoryEnabled) {
                logger->log(event.context.level, logger_render_output_event(event));
            }
            if (categoryEnabled && activeDisplay != nullptr
                && (event.context.level >= spdlog::level::warn || consoleOutputEnabled.load(std::memory_order_acquire))) {
                activeDisplay->onMessage(logger_format_message(event.context));
            }
            if (event.context.level == spdlog::level::critical) {
                logger->dump_backtrace();
            }
            break;
        case OutputAction::DIAGNOSTIC:
            if (categoryEnabled) {
                logger->log(event.context.level, logger_render_output_event(event));
            }
            if (activeDisplay != nullptr && event.context.mirrorToDisplay) {
                routeToDisplay(event);
            }
            break;
        case OutputAction::STDOUT:
            if (activeDisplay != nullptr) {
                routeToDisplay(event);
            } else {
                std::cout << logger_render_output_event(event);
                if (logger_stdout_needs_trailing_newline(event.context)) {
                    std::cout << '\n';
                }
                std::cout.flush();
            }
            break;
        case OutputAction::PLUGIN_STATUS:
        case OutputAction::PLUGIN_PROGRESS:
        case OutputAction::PLUGIN_EVENT:
        case OutputAction::PLUGIN_ARTIFACT:
            if (activeDisplay != nullptr) {
                routeToDisplay(event);
            } else {
                std::cout << logger_render_output_event(event) << '\n';
                std::cout.flush();
            }
            break;
        case OutputAction::DISPLAY_SESSION_BEGIN:
        case OutputAction::DISPLAY_SESSION_END:
        case OutputAction::DISPLAY_ITEM_BEGIN:
        case OutputAction::DISPLAY_ITEM_PROGRESS:
        case OutputAction::DISPLAY_ITEM_STEP:
        case OutputAction::DISPLAY_ITEM_SUCCESS:
        case OutputAction::DISPLAY_ITEM_FAILURE:
        case OutputAction::DISPLAY_MESSAGE:
        case OutputAction::DISPLAY_TABLE_HEADER:
        case OutputAction::DISPLAY_TABLE_ROW:
        case OutputAction::DISPLAY_TABLE_END:
            if (activeDisplay != nullptr) {
                routeToDisplay(event);
            }
            break;
        case OutputAction::FLUSH:
        case OutputAction::STOP:
            if (activeDisplay != nullptr) {
                activeDisplay->flush();
            }
            for (const auto& sink : sinks) {
                sink->flush();
            }
            {
                std::lock_guard<std::mutex> lock(settingsMutex);
                if (structuredFileStream.is_open()) {
                    structuredFileStream.flush();
                }
            }
            std::cout.flush();
            break;
    }
}
