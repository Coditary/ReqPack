#pragma once

namespace reqpack::terminal {

/// True when stdout is an interactive terminal (not a pipe or file).
bool is_stdout_tty();

/// True when styled / animated output should run (TTY and NO_COLOR unset).
bool use_animated_terminal_ui();

}  // namespace reqpack::terminal
