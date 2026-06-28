# Bash completion for ucc-cli
# Installed to /usr/share/bash-completion/completions/ucc-cli
#
# shellcheck shell=bash
# shellcheck disable=SC2207,SC2034
# SC2207: COMPREPLY=( $(compgen ...) ) is the idiomatic bash-completion pattern.
# SC2034: completion helpers receive `prev`/`words`/`cword` from the framework.

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

    local commands="status monitor cpu gpu power-limits profile statemap fan brightness webcam watercooler charging help version"

    # Sub-commands per top-level command
    local profile_cmds="list get set defaults customs apply save delete"
    local statemap_cmds="get set"
    local fan_cmds="list get set apply revert"
    local brightness_cmds="get set"
    local webcam_cmds="get set"
    local watercooler_cmds="status enable disable fan pump led led-off"
    local charging_cmds="status set-profile set-priority set-thresholds"

    # Global flags
    local global_flags="--json --help --version"

    # Find the top-level command (skip flags)
    local cmd="" subcmd="" cmdidx=0
    for (( i=1; i < cword; i++ )); do
        case "${words[i]}" in
            --json|--help|--version|-h|-v) ;;
            *)
                if [[ -z "$cmd" ]]; then
                    cmd="${words[i]}"
                    cmdidx=$i
                elif [[ -z "$subcmd" ]]; then
                    subcmd="${words[i]}"
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
            brightness|br)
                COMPREPLY=( $(compgen -W "$brightness_cmds" -- "$cur") )
                return ;;
            webcam)
                COMPREPLY=( $(compgen -W "$webcam_cmds" -- "$cur") )
                return ;;
            watercooler|wc)
                COMPREPLY=( $(compgen -W "$watercooler_cmds" -- "$cur") )
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
        webcam)
            case "$subcmd" in
                set) COMPREPLY=( $(compgen -W "on off" -- "$cur") ); return ;;
            esac
            ;;
        watercooler|wc)
            case "$subcmd" in
                enable|on|disable|off|fan|pump|led|led-off) ;;  # no further completion
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
                        COMPREPLY=( $(compgen -W "ac ac_wc" -- "$cur") )
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
