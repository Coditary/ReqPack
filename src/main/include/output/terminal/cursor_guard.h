#pragma once

#include "output/terminal/tty.h"

#include <indicators/cursor_control.hpp>

namespace reqpack::terminal {

/// Hides the terminal cursor for the lifetime of this guard (TTY only).
class CursorGuard {
public:
	CursorGuard();
	~CursorGuard();

	CursorGuard(const CursorGuard&) = delete;
	CursorGuard& operator=(const CursorGuard&) = delete;

private:
	bool active_{false};
};

}  // namespace reqpack::terminal
