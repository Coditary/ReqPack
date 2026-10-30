# rqp bash completion — Shared-Cpp command tree via `rqp --complete-subcommands`.

_rqp_cmd() {
    if [[ -n "${RQPACK_COMPLETION_RQP:-}" ]]; then
        printf '%s\n' "$RQPACK_COMPLETION_RQP"
        return 0
    fi
    command -v rqp 2>/dev/null
}

_rqp_subcommand_completions() {
    local prefix="$1"
    local rqp_cmd
    rqp_cmd="$(_rqp_cmd)"
    [[ -z "$rqp_cmd" ]] && return 0
    "$rqp_cmd" --complete-subcommands "$prefix" 2>/dev/null
}

_rqp_global_flag_completions() {
    local prefix="$1"
    local rqp_cmd
    rqp_cmd="$(_rqp_cmd)"
    [[ -z "$rqp_cmd" ]] && return 0
    "$rqp_cmd" --complete-global-flags "$prefix" 2>/dev/null
}

_rqp_first_command_index() {
    local index=1
    while (( index < COMP_CWORD )); do
        local word="${COMP_WORDS[index]}"
        case "$word" in
            --config|-c|--registry|--plugin-dir|--jobs|-j|--osv-db|--osv-feed|--osv-refresh|--ignore-vuln|--sbom-format|--report-format|--archive-password|--on-unsafe|--on-unresolved|--severity-threshold|--score-threshold)
                ((index += 2))
                continue
                ;;
            --*)
                ((index += 1))
                continue
                ;;
            -?)
                if [[ "$word" == "-j" || "$word" == "-c" ]]; then
                    ((index += 2))
                else
                    ((index += 1))
                fi
                continue
                ;;
        esac
        printf '%s\n' "$index"
        return 0
    done
    return 1
}

_rqp_dotted_prefix() {
    local cmd_index="$1"
    local prefix=""
    local index

    if (( COMP_CWORD <= cmd_index )); then
        printf '%s\n' ""
        return 0
    fi

    if (( COMP_CWORD == cmd_index + 1 )); then
        printf '%s\n' "${COMP_WORDS[COMP_CWORD]}"
        return 0
    fi

    for ((index = cmd_index; index < COMP_CWORD - 1; ++index)); do
        prefix+="${COMP_WORDS[index]}."
    done
    prefix+="${COMP_WORDS[COMP_CWORD]}"
    printf '%s\n' "$prefix"
}

_rqp() {
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local cmd_index prefix match suffix

    if [[ "$cur" == -* ]]; then
        mapfile -t COMPREPLY < <(compgen -W "$(_rqp_global_flag_completions "$cur")" -- "$cur")
        return 0
    fi

    if ! cmd_index="$(_rqp_first_command_index)"; then
        mapfile -t COMPREPLY < <(compgen -W "$(_rqp_global_flag_completions "$cur")" -- "$cur")
        return 0
    fi

    prefix="$(_rqp_dotted_prefix "$cmd_index")"
    while IFS= read -r match; do
        [[ -z "$match" ]] && continue
        if [[ "$match" == *.* ]]; then
            suffix="${match##*.}"
        else
            suffix="$match"
        fi
        COMPREPLY+=("$suffix")
    done < <(_rqp_subcommand_completions "$prefix")
}

if [[ "${BASH_SOURCE[0]}" != "${0}" ]]; then
    complete -F _rqp rqp
fi
