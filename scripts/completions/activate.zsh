# Source rqp zsh completion from the user config directory.
if [[ -f "${XDG_CONFIG_HOME:-$HOME/.config}/reqpack/completions/rqp.zsh" ]]; then
    # shellcheck source=/dev/null
    source "${XDG_CONFIG_HOME:-$HOME/.config}/reqpack/completions/rqp.zsh"
fi
