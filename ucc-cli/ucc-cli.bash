# Bash completion for ucc-cli
# Installed to /usr/share/bash-completion/completions/ucc-cli

# Helper: extract IDs from ucc-cli list output (first column, %-36s padded)
# Format: "  <ID>  <name>" with 2+ spaces separating the columns.
_ucc_cli_ids()
{
    local line id
    while IFS= read -r line; do
        [[ "$line" == \ \ [!\ ]* ]] || continue   # starts with "  " + non-space
        id="${line#  }"        # strip leading 2 spaces
        id="${id%%  *}"        # strip from first run of 2+ spaces onward
        [[ -n "$id" ]] && echo "$id"
    done
}

_ucc_cli()
{
    local cur prev words cword
    _init_completion || return

    local commands="status monitor cpu gpu power-limits odm profile statemap fan keyboard brightness webcam fnlock charging help version"

    # Sub-commands per top-level command
    local profile_cmds="list get set defaults customs apply save delete"
    local statemap_cmds="get set"
    local fan_cmds="list get set apply revert"
    local gpu_cmds="info profile oc-state auto-oc"
    local gpu_profile_cmds="list get set reset"
    local gpu_autooc_cmds="core vram both stop status"
    local odm_cmds="list get set"
    local keyboard_cmds="info get set color profiles activate"
    local brightness_cmds="get set"
    local webcam_cmds="get set"
    local fnlock_cmds="get set"
    local charging_cmds="status set-profile set-priority set-thresholds"

    # Global flags
    local global_flags="--json --help --version"

    # Find the top-level command (skip flags)
    local cmd="" subcmd="" sub2cmd="" cmdidx=0
    for (( i=1; i < cword; i++ )); do
        case "${words[i]}" in
            --json|--help|--version|-h|-v) ;;
            *)
                if [[ -z "$cmd" ]]; then
                    cmd="${words[i]}"
                    cmdidx=$i
                elif [[ -z "$subcmd" ]]; then
                    subcmd="${words[i]}"
                elif [[ -z "$sub2cmd" ]]; then
                    sub2cmd="${words[i]}"
                fi
                ;;
        esac
    done

    # No command yet — complete top-level commands + global flags
    if [[ -z "$cmd" ]]; then
        COMPREPLY=( $(compgen -W "$commands $global_flags" -- "$cur") )
        return
    fi

    # Complete subcommand
    if [[ -z "$subcmd" ]]; then
        case "$cmd" in
            profile|prof)
                COMPREPLY=( $(compgen -W "$profile_cmds" -- "$cur") )
                return ;;
            statemap|state-map)
                COMPREPLY=( $(compgen -W "$statemap_cmds" -- "$cur") )
                return ;;
            fan)
                COMPREPLY=( $(compgen -W "$fan_cmds" -- "$cur") )
                return ;;
            gpu)
                COMPREPLY=( $(compgen -W "$gpu_cmds" -- "$cur") )
                return ;;
            odm)
                COMPREPLY=( $(compgen -W "$odm_cmds" -- "$cur") )
                return ;;
            keyboard|kb)
                COMPREPLY=( $(compgen -W "$keyboard_cmds" -- "$cur") )
                return ;;
            brightness|br)
                COMPREPLY=( $(compgen -W "$brightness_cmds" -- "$cur") )
                return ;;
            webcam)
                COMPREPLY=( $(compgen -W "$webcam_cmds" -- "$cur") )
                return ;;
            fnlock|fn-lock)
                COMPREPLY=( $(compgen -W "$fnlock_cmds" -- "$cur") )
                return ;;
            charging|charge)
                COMPREPLY=( $(compgen -W "$charging_cmds" -- "$cur") )
                return ;;
            monitor|mon)
                COMPREPLY=( $(compgen -W "-n -i" -- "$cur") )
                return ;;
        esac
        return
    fi

    # Complete arguments to subcommands
    case "$cmd" in
        profile|prof)
            case "$subcmd" in
                set|activate|delete|del|rm)
                    # Complete with profile IDs from daemon (built-in + custom)
                    local -a ids
                    mapfile -t ids < <(ucc-cli profile list 2>/dev/null | _ucc_cli_ids)
                    local IFS=$'\n'
                    COMPREPLY=( $(compgen -W "${ids[*]}" -- "$cur") )
                    compopt -o filenames   # handle quoting / escaping
                    return ;;
            esac
            ;;
        fan)
            case "$subcmd" in
                get|show|set|activate)
                    # Complete with fan profile IDs from daemon
                    local -a ids
                    mapfile -t ids < <(ucc-cli fan list 2>/dev/null | _ucc_cli_ids)
                    local IFS=$'\n'
                    COMPREPLY=( $(compgen -W "${ids[*]}" -- "$cur") )
                    return ;;
            esac
            ;;
        gpu)
            case "$subcmd" in
                profile|prof)
                    if [[ -z "$sub2cmd" ]]; then
                        COMPREPLY=( $(compgen -W "$gpu_profile_cmds" -- "$cur") )
                        return
                    fi
                    case "$sub2cmd" in
                        get|show|set|activate|apply)
                            # Complete with GPU OC profile IDs
                            local -a ids
                            mapfile -t ids < <(ucc-cli gpu profile list 2>/dev/null | _ucc_cli_ids)
                            local IFS=$'\n'
                            COMPREPLY=( $(compgen -W "${ids[*]}" -- "$cur") )
                            return ;;
                    esac
                    ;;
                auto-oc|autooc|auto_oc)
                    if [[ -z "$sub2cmd" ]]; then
                        COMPREPLY=( $(compgen -W "$gpu_autooc_cmds" -- "$cur") )
                        return
                    fi
                    ;;
            esac
            ;;
        odm)
            case "$subcmd" in
                set)
                    # Complete with available ODM profile names
                    local -a profiles
                    mapfile -t profiles < <(ucc-cli odm list 2>/dev/null | sed -n 's/^  //p')
                    local IFS=$'\n'
                    COMPREPLY=( $(compgen -W "${profiles[*]}" -- "$cur") )
                    return ;;
            esac
            ;;
        keyboard|kb)
            case "$subcmd" in
                activate|use)
                    # Complete with keyboard profile IDs
                    local -a ids
                    mapfile -t ids < <(ucc-cli keyboard profiles 2>/dev/null | _ucc_cli_ids)
                    local IFS=$'\n'
                    COMPREPLY=( $(compgen -W "${ids[*]}" -- "$cur") )
                    return ;;
            esac
            ;;
        webcam)
            case "$subcmd" in
                set) COMPREPLY=( $(compgen -W "on off" -- "$cur") ); return ;;
            esac
            ;;
        fnlock|fn-lock)
            case "$subcmd" in
                set) COMPREPLY=( $(compgen -W "on off" -- "$cur") ); return ;;
            esac
            ;;
        charging|charge)
            case "$subcmd" in
                set-profile|set-priority)
                    COMPREPLY=( $(compgen -W "high_capacity balanced stationary" -- "$cur") )
                    return ;;
            esac
            ;;
        statemap|state-map)
            case "$subcmd" in
                set)
                    # Position after "statemap set": first arg is STATE, second is PROFILE_ID
                    local nargs=0
                    for (( i=cmdidx+2; i < cword; i++ )); do
                        (( nargs++ ))
                    done
                    if [[ $nargs -eq 0 ]]; then
                        COMPREPLY=( $(compgen -W "ac" -- "$cur") )
                    elif [[ $nargs -eq 1 ]]; then
                        local -a ids
                        mapfile -t ids < <(ucc-cli profile list 2>/dev/null | _ucc_cli_ids)
                        local IFS=$'\n'
                        COMPREPLY=( $(compgen -W "${ids[*]}" -- "$cur") )
                        compopt -o filenames
                    fi
                    return ;;
            esac
            ;;
    esac
}

complete -F _ucc_cli ucc-cli
