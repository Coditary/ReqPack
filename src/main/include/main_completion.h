#pragma once

#include <optional>

/// Handles `--complete-subcommands`, `--dump-completion`, `--install-completion` before normal startup.
[[nodiscard]] std::optional<int> try_handle_completion_cli(int argc, char* argv[]);
