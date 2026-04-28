#pragma once

#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// ansi_color — convert human-readable color spec strings into ANSI escape
// sequences.
//
// Supported spec format (space-separated tokens, order doesn't matter):
//   modifiers : "bold"  "dim"  "underline"
//   colors    : "black"  "red"  "green"  "yellow"  "blue"
//               "magenta"  "cyan"  "white"
//   bright    : "bright_black"  "bright_red"  "bright_green"  "bright_yellow"
//               "bright_blue"  "bright_magenta"  "bright_cyan"  "bright_white"
//   special   : ""  "none"  → no-op (returns empty string)
//
// Examples
//   "bold green"       → "\033[1;32m"
//   "bright_cyan"      → "\033[96m"
//   "bold bright_red"  → "\033[1;91m"
//   ""                 → ""
// ─────────────────────────────────────────────────────────────────────────────

/// Returns the opening ANSI escape for the given spec, or "" if spec is empty/none.
std::string ansi_open(const std::string& spec);

/// Returns the ANSI reset sequence "\033[0m", or "" if spec is empty/none
/// (avoids inserting unnecessary resets in plain-text mode).
std::string ansi_reset(const std::string& spec);

/// Wraps text with ansi_open(spec) … ansi_reset(spec).
/// Returns text unchanged when spec is empty or "none".
std::string ansi_wrap(const std::string& text, const std::string& spec);
