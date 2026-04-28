#pragma once

#include "core/configuration.h"
#include "output/idisplay.h"

#include <memory>

// ─────────────────────────────────────────────────────────────────────────────
// create_display — factory that instantiates the correct IDisplay implementation
// based on the DisplayConfig loaded from the Lua configuration file.
//
// Ownership:
//   The caller (main.cpp) owns the returned unique_ptr.  The raw pointer
//   passed to Logger::setDisplay() is valid for the lifetime of the
//   unique_ptr — ensure the display outlives the Logger worker thread.
//
// Supported renderers:
//   DisplayRenderer::PLAIN  → PlainDisplay  (default; no ANSI codes)
//   DisplayRenderer::COLOR  → ColorDisplay  (ANSI colors from DisplayColorScheme)
// ─────────────────────────────────────────────────────────────────────────────
std::unique_ptr<IDisplay> create_display(const DisplayConfig& config);
