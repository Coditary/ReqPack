#pragma once

#include <ostream>
#include <string>

namespace reqpack::terminal {

inline constexpr const char* kEraseLine = "\x1b[K";
inline constexpr const char* kCarriageReturn = "\r";
inline constexpr const char* kHideCursor = "\x1b[?25l";
inline constexpr const char* kShowCursor = "\x1b[?25h";
inline constexpr const char* kDim = "\x1b[2m";
inline constexpr const char* kReset = "\x1b[0m";
inline constexpr const char* kGreen = "\x1b[32m";
inline constexpr const char* kCyan = "\x1b[36m";
inline constexpr const char* kBrightBlack = "\x1b[90m";

void write_erase_line(std::ostream& stream);
void write_carriage_return_erase(std::ostream& stream);

/// Move the cursor up `lines` rows (no-op when lines <= 0).
void move_cursor_up(std::ostream& stream, int lines);

/// OSC-8 hyperlink (no-op when url or label is empty, or hyperlinks are disabled).
std::string osc8_hyperlink(const std::string& url, const std::string& label, bool enableHyperlinks);

}  // namespace reqpack::terminal
