#pragma once

#include "output/plain_display.h"

#include <cstddef>
#include <string>
#include <vector>

namespace plain_display_internal {

std::string join_items(const std::vector<std::string>& values);
std::string repeat_char(char value, int count);
std::string pad_right(const std::string& text, size_t width);
void merge_progress_metrics(DisplayProgressMetrics& target, const DisplayProgressMetrics& update);
size_t terminal_width();
std::string truncate_middle(const std::string& text, size_t width);
std::vector<std::string> split_long_token(const std::string& token, size_t width);

}  // namespace plain_display_internal
