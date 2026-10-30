#compdef rqp
# rqp zsh completion — Shared-Cpp command tree via `rqp --complete-subcommands`.

_rqp_cmd() {
    if [[ -n ${RQPACK_COMPLETION_RQP:-} ]]; then
        print -r -- "$RQPACK_COMPLETION_RQP"
        return 0
    fi
    command -v rqp 2>/dev/null
}

_rqp_subcommand_completions() {
    local prefix=$1
    local rqp_cmd
    rqp_cmd=$(_rqp_cmd)
    [[ -z $rqp_cmd ]] && return 0
    "$rqp_cmd" --complete-subcommands "$prefix" 2>/dev/null
}

_rqp_global_flag_completions() {
    local prefix=$1
    local rqp_cmd
    rqp_cmd=$(_rqp_cmd)
    [[ -z $rqp_cmd ]] && return 0
    "$rqp_cmd" --complete-global-flags "$prefix" 2>/dev/null
}

_rqp_first_command_index() {
    local index=1
    while (( index < CURRENT )); do
        local word=${words[index]}
        case $word in
            --config|-c|--registry|--plugin-dir|--jobs|-j|--osv-db|--osv-feed|--osv-refresh|--ignore-vuln|--sbom-format|--report-format|--archive-password|--on-unsafe|--on-unresolved|--severity-threshold|--score-threshold)
                ((index += 2))
                continue
                ;;
            --*)
                ((index += 1))
                continue
                ;;
            -?)
                if [[ $word == -j || $word == -c ]]; then
                    ((index += 2))
                else
                    ((index += 1))
                fi
                continue
                ;;
        esac
        print -r -- $index
        return 0
    done
    return 1
}

_rqp_dotted_prefix() {
    local cmd_index=$1
    local prefix=""
    local index

    if (( CURRENT <= cmd_index + 1 )); then
        print -r -- $words[CURRENT]
        return 0
    fi

    for ((index = cmd_index; index < CURRENT - 1; ++index)); do
        prefix+="${words[index]}."
    done
    prefix+=$words[CURRENT]
    print -r -- $prefix
}

_rqp() {
    local cmd_index prefix match suffix
    local -a suggestions

    if [[ $words[CURRENT] == -* ]]; then
        suggestions=(${(f)"$(_rqp_global_flag_completions "$words[CURRENT]")"})
        compadd -- $suggestions
        return 0
    fi

    if ! cmd_index=$(_rqp_first_command_index); then
        suggestions=(${(f)"$(_rqp_global_flag_completions "$words[CURRENT]")"})
        compadd -- $suggestions
        return 0
    fi

    prefix=$(_rqp_dotted_prefix $cmd_index)
    suggestions=(${(f)"$(_rqp_subcommand_completions "$prefix")"})
    for match in $suggestions; do
        if [[ $match == *.* ]]; then
            suffix=${match##*.}
        else
            suffix=$match
        fi
        compadd -- $suffix
    done
}

compdef _rqp rqp
