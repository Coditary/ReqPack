#pragma once

#include "output/idisplay.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace plain_display_internal {

/// Shared CLI lexicon (no emoji); used by PlainDisplay, LiveDisplay, and ColorDisplay.
inline constexpr const char* kIndent = "  ";
inline constexpr const char* kSuccessMark = "\xe2\x9c\x93";   // ✓
inline constexpr const char* kSkippedMark = "-";
inline constexpr const char* kInfoMark = "i";
inline constexpr const char* kLinkMark = "\xe2\x86\x92";      // →
inline constexpr const char* kWarningMark = "!";
inline constexpr const char* kErrorMark = "\xe2\x9c\x97";     // ✗
inline constexpr const char* kBarDoneUnit = "\xe2\x94\x81";   // ━
inline constexpr const char* kBarTodoUnit = "\xe2\x94\x80";   // ─

std::string join_items(const std::vector<std::string>& values);
/// Comma-separated session list; caps long lists as "N packages".
std::string format_session_item_list(const std::vector<std::string>& values);
std::string action_banner_text(DisplayMode mode, const std::vector<std::string>& items);
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
enum class TranscriptLineDecoration { None, Info, Link };
bool is_transaction_chatter_message(const std::string& text);
TranscriptLineDecoration transcript_line_decoration(std::string_view trimmed);
std::optional<std::string> session_summary_text(DisplayMode mode,
                                                bool success,
                                                int succeeded,
                                                int skipped,
                                                int failed);
std::string format_item_action_line(const char* marker, const std::string& body);
std::string item_outcome_phrase(DisplayMode mode);
bool progress_has_byte_transfer(const DisplayProgressMetrics& metrics);
std::optional<std::string> common_session_system(const std::vector<std::string>& itemIds);
bool is_generic_plugin_failure_summary(const std::string& reason);
std::string format_item_failure_primary(const std::string& itemId, const std::string& reason);
std::optional<std::string> failure_hint_for_reason(const std::string& itemId, const std::string& reason);
std::string format_item_success_body(const std::string& itemId, DisplayMode mode, const std::string& version);
void write_session_summary(std::ostream& stream, DisplayMode mode, int succeeded, int skipped, int failed);

}  // namespace plain_display_internal
