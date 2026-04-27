#pragma once

#include "output/logger.h"

#include <string>

std::string logger_format_message(const OutputContext& context);
std::string logger_render_output_event(const OutputEvent& event);
bool logger_stdout_needs_trailing_newline(const OutputContext& context);
