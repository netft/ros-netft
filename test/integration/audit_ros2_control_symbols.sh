#!/usr/bin/env bash
set -euo pipefail

if (( $# == 0 )); then
  echo "usage: $0 PLUGIN_DSO..." >&2
  exit 2
fi

nm_tool="${NM:-nm}"
command -v "$nm_tool" >/dev/null

for plugin in "$@"; do
  test -f "$plugin"
  symbols="$("$nm_tool" -D --defined-only --demangle "$plugin")"
  if ! grep -q 'netft_driver::NetFTHardwareInterface' <<<"$symbols"; then
    echo "production ros2_control plugin export is missing: $plugin" >&2
    exit 1
  fi
  private_symbols="$(
    sed -E 's/^[[:xdigit:]]+[[:space:]]+[[:alpha:]][[:space:]]+//' <<<"$symbols" |
      grep -E \
        '^(netft::|(typeinfo|typeinfo name|vtable|VTT) for netft::|netft_driver::(DiagnosticEvaluator|FaultLogThrottle|force_scale_to_newtons|torque_scale_to_newton_metres|to_si_sample)\b)' \
        || true
  )"
  if [[ -n "$private_symbols" ]]; then
    echo "private core/support symbols exported by $plugin:" >&2
    printf '%s\n' "$private_symbols" >&2
    exit 1
  fi
done
