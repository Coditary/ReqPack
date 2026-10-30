#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPLETIONS_SRC="${ROOT}/completions"

# shellcheck source=completions/install-lib.sh
source "${COMPLETIONS_SRC}/install-lib.sh"

install_rqp_completion_files "$COMPLETIONS_SRC"
install_rqp_completion_hooks
print_rqp_completion_paths

echo "Restart your shell or run: source ~/.config/reqpack/activate.bash  (or activate.zsh)"
