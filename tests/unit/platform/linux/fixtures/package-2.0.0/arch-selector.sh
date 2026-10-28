# Historical fixture: Nonary/Vibepollo b3b3dbf44 (published 2.0.0).
# Extracted verbatim function definitions; never source the full host-mutating hook.
vibepollo_privileged_helper_is_safe() {
  [[ -f "$1" && ! -L "$1" && -x "$1" &&
     $(stat -c '%u:%g:%a:%h:%F' -- "$1") == \
       '0:0:755:1:regular file' ]]
}

vibepollo_select_upgrade_kill_mode() {
  if [[ ! -e "$vibepollo_legacy_host" && ! -L "$vibepollo_legacy_host" ]]; then
    vibepollo_unit_is_quiescent vibepollo.service || return 1
    vibepollo_upgrade_kill_mode=control-group
    return 0
  fi
  vibepollo_privileged_helper_is_safe "$vibepollo_legacy_host" || return 1
  if grep -Fqx '  trap request_host_shutdown TERM INT HUP' "$vibepollo_legacy_host"; then
    vibepollo_upgrade_kill_mode=mixed
  elif grep -Fqx '  trap mark_host_shutdown TERM INT HUP' "$vibepollo_legacy_host"; then
    vibepollo_upgrade_kill_mode=control-group
  elif grep -Fqx "  trap 'forward_host_signal TERM' TERM" "$vibepollo_legacy_host" && \
       grep -Fqx "  trap 'forward_host_signal INT' INT" "$vibepollo_legacy_host" && \
       grep -Fqx "  trap 'forward_host_signal HUP' HUP" "$vibepollo_legacy_host"; then
    vibepollo_upgrade_kill_mode=process
  else
    return 1
  fi
}
