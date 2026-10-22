#pragma once

#include "output/idisplay.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace plain_display_internal {

std::string join_items(const std::vector<std::string>& values);
std::string repeat_char(char value, int count);
std::string repeat_visual_unit(const std::string& unit, size_t count);
std::string pad_right(const std::string& text, size_t width);
void merge_progress_metrics(DisplayProgressMetrics& target, const DisplayProgressMetrics& update);
size_t terminal_width();
bool stdout_is_tty();
size_t visual_length(const std::string& text);
std::string truncate_middle(const std::string& text, size_t width);
std::string truncate_end(const std::string& text, size_t width);
std::string pad_right_visual(const std::string& text, size_t width);
void write_padded_cell(std::ostream& stream, const std::string& segment, size_t width);
std::vector<std::string> split_long_token(const std::string& token, size_t width);
std::string trim_trailing_newlines(std::string_view value);
std::optional<std::string> session_summary_text(DisplayMode mode,
                                                bool success,
                                                int succeeded,
                                                int skipped,
                                                int failed);

}  // namespace plain_display_internal
