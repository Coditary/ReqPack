#!/usr/bin/env bash
# Shared helpers for installing rqp shell completion.

rqp_completion_paths() {
    CONFIG_DIR="${XDG_CONFIG_HOME:-"$HOME/.config"}/reqpack"
    USER_COMPLETION_DIR="${CONFIG_DIR}/completions"
    BASH_COMPLETION_DIR="${RQPACK_BASH_COMPLETION_DIR:-"${XDG_DATA_HOME:-"$HOME/.local/share"}/bash-completion/completions"}"
    ZSH_COMPLETION_DIR="${RQPACK_ZSH_COMPLETION_DIR:-"${XDG_DATA_HOME:-"$HOME/.local/share"}/zsh/site-functions"}"
    BASH_COMPLETION_TARGET="${BASH_COMPLETION_DIR}/rqp"
    ZSH_COMPLETION_TARGET="${ZSH_COMPLETION_DIR}/_rqp"
}

install_rqp_completion_files() {
    local completions_src="$1"

    rqp_completion_paths
    mkdir -p "$CONFIG_DIR" "$USER_COMPLETION_DIR" "$BASH_COMPLETION_DIR" "$ZSH_COMPLETION_DIR"

    cp -f "${completions_src}/rqp.bash" "${USER_COMPLETION_DIR}/rqp.bash"
    cp -f "${completions_src}/rqp.zsh" "${USER_COMPLETION_DIR}/rqp.zsh"
    cp -f "${completions_src}/activate.bash" "${USER_COMPLETION_DIR}/activate.bash"
    cp -f "${completions_src}/activate.zsh" "${USER_COMPLETION_DIR}/activate.zsh"
    cp -f "${completions_src}/install-lib.sh" "${USER_COMPLETION_DIR}/install-lib.sh"

    ln -sf "${USER_COMPLETION_DIR}/activate.zsh" "${CONFIG_DIR}/activate.zsh"
    ln -sf "${USER_COMPLETION_DIR}/activate.bash" "${CONFIG_DIR}/activate.bash"
    ln -sf "${USER_COMPLETION_DIR}/rqp.bash" "$BASH_COMPLETION_TARGET"
    ln -sf "${USER_COMPLETION_DIR}/rqp.zsh" "$ZSH_COMPLETION_TARGET"
}

install_rqp_completion_hooks() {
    local hook_begin="# >>> rqp shell completion >>>"
    local hook_end="# <<< rqp shell completion <<<"
    local zsh_hook='[[ -f "${XDG_CONFIG_HOME:-$HOME/.config}/reqpack/activate.zsh" ]] && source "${XDG_CONFIG_HOME:-$HOME/.config}/reqpack/activate.zsh"'
    local bash_hook='[[ -f "${XDG_CONFIG_HOME:-$HOME/.config}/reqpack/activate.bash" ]] && source "${XDG_CONFIG_HOME:-$HOME/.config}/reqpack/activate.bash"'

    _install_rqp_shell_hook "$HOME/.zshrc" "$hook_begin" "$hook_end" "$zsh_hook"
    _install_rqp_shell_hook "$HOME/.bashrc" "$hook_begin" "$hook_end" "$bash_hook"
}

_install_rqp_shell_hook() {
    local rc_file="$1"
    local hook_begin="$2"
    local hook_end="$3"
    local hook_line="$4"

    [[ -f "$rc_file" ]] || return 0
    if grep -qF "$hook_begin" "$rc_file"; then
        echo "Shell hook already present in ${rc_file}"
        return 0
    fi

    {
        echo ""
        echo "$hook_begin"
        echo "$hook_line"
        echo "$hook_end"
    } >>"$rc_file"

    echo "Added shell hook to ${rc_file}"
}

print_rqp_completion_paths() {
    rqp_completion_paths
    echo "  user: ${USER_COMPLETION_DIR}"
    echo "  bash: ${BASH_COMPLETION_TARGET}"
    echo "  zsh:  ${ZSH_COMPLETION_TARGET}"
}
