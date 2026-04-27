#include "output/logger_core.h"

std::string logger_format_message(const OutputContext& context) {
    std::string message;
    if (!context.source.empty()) {
        message += "[" + context.source + "] ";
    }
    if (!context.scope.empty()) {
        message += "(" + context.scope + ") ";
    }
    message += context.message;
    return message;
}

std::string logger_render_output_event(const OutputEvent& event) {
    switch (event.action) {
        case OutputAction::STDOUT:
        case OutputAction::LOG:
            return logger_format_message(event.context);
        case OutputAction::PLUGIN_STATUS:
            return logger_format_message(OutputContext{
                .level = spdlog::level::info,
                .message = "status=" + std::to_string(event.context.statusCode),
                .source = event.context.source,
                .scope = event.context.scope,
            });
        case OutputAction::PLUGIN_PROGRESS:
            return logger_format_message(OutputContext{
                .level = spdlog::level::info,
                .message = "progress=" + std::to_string(event.context.progressPercent) + "%",
                .source = event.context.source,
                .scope = event.context.scope,
            });
        case OutputAction::PLUGIN_EVENT:
            return logger_format_message(OutputContext{
                .level = spdlog::level::info,
                .message = event.context.eventName + ": " + event.context.payload,
                .source = event.context.source,
                .scope = event.context.scope,
            });
        case OutputAction::PLUGIN_ARTIFACT:
            return logger_format_message(OutputContext{
                .level = spdlog::level::info,
                .message = "artifact: " + event.context.payload,
                .source = event.context.source,
                .scope = event.context.scope,
            });
        case OutputAction::FLUSH:
        case OutputAction::STOP:
            return {};
    }

    return {};
}

bool logger_stdout_needs_trailing_newline(const OutputContext& context) {
    return context.message.empty() || context.message.back() != '\n';
}
