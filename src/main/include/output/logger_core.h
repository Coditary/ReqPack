#pragma once

#include "output/diagnostic.h"
#include "output/logger.h"

#include <string>

std::string logger_escape_json(const std::string& value);
std::string logger_diagnostic_text(const OutputContext& context, bool includePrefix = true);
std::string logger_render_structured_event_json(const OutputEvent& event);
std::string logger_render_display_message(const DiagnosticMessage& diagnostic, bool includeSummary = true);
std::string logger_format_message(const OutputContext& context);
std::string logger_render_output_event(const OutputEvent& event);
bool logger_stdout_needs_trailing_newline(const OutputContext& context);
