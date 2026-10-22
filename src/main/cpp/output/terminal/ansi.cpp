#include "output/terminal/ansi.h"

namespace reqpack::terminal {

void write_erase_line(std::ostream& stream) {
	stream << kEraseLine;
}

void write_carriage_return_erase(std::ostream& stream) {
	stream << kCarriageReturn << kEraseLine;
}

void move_cursor_up(std::ostream& stream, int lines) {
	if (lines <= 0) {
		return;
	}
	stream << "\x1b[" << lines << 'A';
}

std::string osc8_hyperlink(const std::string& url, const std::string& label, const bool enableHyperlinks) {
	if (!enableHyperlinks || url.empty() || label.empty()) {
		return label;
	}
	return "\033]8;;" + url + "\033\\" + label + "\033]8;;\033\\";
}

}  // namespace reqpack::terminal
