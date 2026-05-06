#include "output/logger_core.h"

#include <sstream>

namespace {

std::string level_to_string(spdlog::level::level_enum level) {
	switch (level) {
		case spdlog::level::trace: return "trace";
		case spdlog::level::debug: return "debug";
		case spdlog::level::info: return "info";
		case spdlog::level::warn: return "warn";
		case spdlog::level::err: return "error";
		case spdlog::level::critical: return "critical";
		default: return "info";
	}
}

std::string action_to_event_name(OutputAction action) {
	switch (action) {
		case OutputAction::LOG: return "log";
		case OutputAction::DIAGNOSTIC: return "diagnostic";
		case OutputAction::STDOUT: return "stdout";
		case OutputAction::PLUGIN_STATUS: return "plugin_status";
		case OutputAction::PLUGIN_PROGRESS: return "plugin_progress";
		case OutputAction::PLUGIN_EVENT: return "plugin_event";
		case OutputAction::PLUGIN_ARTIFACT: return "plugin_artifact";
		case OutputAction::DISPLAY_SESSION_BEGIN: return "session_begin";
		case OutputAction::DISPLAY_SESSION_END: return "session_end";
		case OutputAction::DISPLAY_ITEM_BEGIN: return "item_begin";
		case OutputAction::DISPLAY_ITEM_STEP: return "item_step";
		case OutputAction::DISPLAY_ITEM_SUCCESS: return "item_success";
		case OutputAction::DISPLAY_ITEM_FAILURE: return "item_failure";
		case OutputAction::DISPLAY_MESSAGE: return "display_message";
		case OutputAction::DISPLAY_TABLE_HEADER: return "table_header";
		case OutputAction::DISPLAY_TABLE_ROW: return "table_row";
		case OutputAction::DISPLAY_TABLE_END: return "table_end";
		case OutputAction::FLUSH: return "flush";
		case OutputAction::STOP: return "stop";
	}
	return "unknown";
}

} // namespace

std::string logger_escape_json(const std::string& value) {
	std::string escaped;
	escaped.reserve(value.size());
	for (const char c : value) {
		switch (c) {
			case '"': escaped += "\\\""; break;
			case '\\': escaped += "\\\\"; break;
			case '\n': escaped += "\\n"; break;
			case '\r': escaped += "\\r"; break;
			case '\t': escaped += "\\t"; break;
			default: escaped.push_back(c); break;
		}
	}
	return escaped;
}

std::string logger_render_display_message(const DiagnosticMessage& diagnostic, bool includeSummary) {
	std::ostringstream stream;
	bool emitted = false;
	if (includeSummary && !diagnostic.summary.empty()) {
		stream << diagnostic.summary;
		emitted = true;
	}
	if (!diagnostic.cause.empty()) {
		if (emitted) {
			stream << '\n';
		}
		stream << "Cause: " << diagnostic.cause;
		emitted = true;
	}
	if (!diagnostic.recommendation.empty()) {
		if (emitted) {
			stream << '\n';
		}
		stream << "Fix: " << diagnostic.recommendation;
		emitted = true;
	}
	if (!diagnostic.details.empty()) {
		if (emitted) {
			stream << '\n';
		}
		stream << diagnostic.details;
	}
	return stream.str();
}

std::string logger_diagnostic_text(const OutputContext& context, bool includePrefix) {
	DiagnosticMessage diagnostic{
		.severity = context.level,
		.category = context.category,
		.summary = context.message,
		.cause = context.cause,
		.recommendation = context.recommendation,
		.details = context.details,
		.source = context.source,
		.scope = context.scope,
		.context = context.contextFields,
	};
	const std::string rendered = logger_render_display_message(diagnostic, true);
	if (!includePrefix) {
		return rendered;
	}
	std::ostringstream stream;
	if (!context.source.empty()) {
		stream << '[' << context.source << "] ";
	}
	if (!context.scope.empty()) {
		stream << '(' << context.scope << ") ";
	}
	stream << rendered;
	return stream.str();
}

std::string logger_format_message(const OutputContext& context) {
	if (!context.cause.empty() || !context.recommendation.empty() || !context.details.empty()) {
		return logger_diagnostic_text(context, true);
	}
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
		case OutputAction::DIAGNOSTIC:
			return logger_format_message(event.context);

		case OutputAction::PLUGIN_STATUS:
			return logger_format_message(OutputContext{
				.level   = spdlog::level::info,
				.message = "status=" + std::to_string(event.context.statusCode),
				.source  = event.context.source,
				.scope   = event.context.scope,
			});

		case OutputAction::PLUGIN_PROGRESS: {
			const std::string summary = format_progress_summary(DisplayProgressMetrics{
				.percent = event.context.progressPercent,
				.currentBytes = event.context.currentBytes,
				.totalBytes = event.context.totalBytes,
				.bytesPerSecond = event.context.bytesPerSecond,
			});
			return logger_format_message(OutputContext{
				.level   = spdlog::level::info,
				.message = summary.empty() ? "progress" : "progress=" + summary,
				.source  = event.context.source,
				.scope   = event.context.scope,
			});
		}

		case OutputAction::PLUGIN_EVENT:
			return logger_format_message(OutputContext{
				.level   = spdlog::level::info,
				.message = event.context.eventName + ": " + event.context.payload,
				.source  = event.context.source,
				.scope   = event.context.scope,
			});

		case OutputAction::PLUGIN_ARTIFACT:
			return logger_format_message(OutputContext{
				.level   = spdlog::level::info,
				.message = "artifact: " + event.context.payload,
				.source  = event.context.source,
				.scope   = event.context.scope,
			});

		// Display actions are handled directly in Logger::routeToDisplay();
		// fall through returns an empty string so no raw output escapes.
		case OutputAction::DISPLAY_SESSION_BEGIN:
			return "[display:session_begin] " + event.context.payload;
		case OutputAction::DISPLAY_SESSION_END:
			return "[display:session_end] " + event.context.payload;
		case OutputAction::DISPLAY_ITEM_BEGIN:
			return "[display:item_begin] "   + event.context.source + " " + event.context.message;
		case OutputAction::DISPLAY_ITEM_STEP:
			return "[display:item_step] "    + event.context.source + " " + event.context.message;
		case OutputAction::DISPLAY_ITEM_SUCCESS:
			return "[display:item_success] " + event.context.source;
		case OutputAction::DISPLAY_ITEM_FAILURE:
			return "[display:item_failure] " + event.context.source + " " + event.context.message;
		case OutputAction::DISPLAY_MESSAGE:
			return "[display:message] " + event.context.message;
		case OutputAction::DISPLAY_TABLE_HEADER:
			return "[display:table_header] " + event.context.payload;
		case OutputAction::DISPLAY_TABLE_ROW:
			return "[display:table_row] "    + event.context.payload;
		case OutputAction::DISPLAY_TABLE_END:
			return "[display:table_end]";

		case OutputAction::FLUSH:
		case OutputAction::STOP:
			return {};
	}

	return {};
}

std::string logger_render_structured_event_json(const OutputEvent& event) {
	if (event.action == OutputAction::FLUSH || event.action == OutputAction::STOP) {
		return {};
	}

	const OutputContext& context = event.context;
	std::ostringstream stream;
	stream << '{';
	stream << "\"timestamp\":\"\"";
	stream << ",\"severity\":\"" << logger_escape_json(level_to_string(context.level)) << "\"";
	stream << ",\"category\":\"" << logger_escape_json(context.category) << "\"";
	stream << ",\"event\":\"" << logger_escape_json(action_to_event_name(event.action)) << "\"";
	stream << ",\"summary\":\"" << logger_escape_json(context.message) << "\"";
	stream << ",\"cause\":\"" << logger_escape_json(context.cause) << "\"";
	stream << ",\"recommendation\":\"" << logger_escape_json(context.recommendation) << "\"";
	stream << ",\"details\":\"" << logger_escape_json(context.details) << "\"";
	stream << ",\"source\":\"" << logger_escape_json(context.source) << "\"";
	stream << ",\"scope\":\"" << logger_escape_json(context.scope) << "\"";
	stream << ",\"context\":{";
	for (std::size_t index = 0; index < context.contextFields.size(); ++index) {
		if (index > 0) {
			stream << ',';
		}
		stream << "\"" << logger_escape_json(context.contextFields[index].first) << "\":\""
		       << logger_escape_json(context.contextFields[index].second) << "\"";
	}
	stream << "}}";
	return stream.str();
}

bool logger_stdout_needs_trailing_newline(const OutputContext& context) {
	return context.message.empty() || context.message.back() != '\n';
}
