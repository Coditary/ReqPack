#include "output/terminal/tty.h"

#include <cstdlib>
#include <unistd.h>

namespace reqpack::terminal {

bool is_stdout_tty() {
	return isatty(STDOUT_FILENO) != 0;
}

bool use_animated_terminal_ui() {
	if (!is_stdout_tty()) {
		return false;
	}
	if (const char* noColor = std::getenv("NO_COLOR"); noColor != nullptr && noColor[0] != '\0') {
		return false;
	}
	return true;
}

}  // namespace reqpack::terminal
