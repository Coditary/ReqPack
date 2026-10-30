# Source rqp bash completion from the user config directory.
if [[ -f "${XDG_CONFIG_HOME:-$HOME/.config}/reqpack/completions/rqp.bash" ]]; then
    # shellcheck source=/dev/null
    source "${XDG_CONFIG_HOME:-$HOME/.config}/reqpack/completions/rqp.bash"
fi
