#include "output/terminal/cursor_guard.h"

namespace reqpack::terminal {

CursorGuard::CursorGuard() {
	if (use_animated_terminal_ui()) {
		indicators::show_console_cursor(false);
		active_ = true;
	}
}

CursorGuard::~CursorGuard() {
	if (active_) {
		indicators::show_console_cursor(true);
	}
}

}  // namespace reqpack::terminal
