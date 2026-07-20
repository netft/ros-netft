#!/usr/bin/env bash
set -euo pipefail

shutdown_poll_attempts=20
shutdown_poll_interval=0.05
shutdown_term_escalation_status=124
shutdown_kill_escalation_status=125
shutdown_survivor_status=126
reaped_pid=""
reaped_status=0

process_state() {
  local pid="$1"
  local stat_line
  local remainder
  [[ -r "/proc/$pid/stat" ]] || return 1
  IFS= read -r stat_line <"/proc/$pid/stat" || return 1
  remainder="${stat_line##*) }"
  [[ "$remainder" != "$stat_line" ]] || return 1
  printf '%s\n' "${remainder%% *}"
}

reap_if_exited() {
  local pid="$1"
  local state
  if [[ "$reaped_pid" == "$pid" ]]; then
    return 0
  fi
  if ! kill -0 "$pid" 2>/dev/null; then
    if wait "$pid" 2>/dev/null; then
      reaped_status=0
    else
      reaped_status=$?
    fi
    reaped_pid="$pid"
    return 0
  fi
  state="$(process_state "$pid" 2>/dev/null)" || return 1
  if [[ "$state" == "Z" ]]; then
    if wait "$pid" 2>/dev/null; then
      reaped_status=0
    else
      reaped_status=$?
    fi
    reaped_pid="$pid"
    return 0
  fi
  return 1
}

wait_for_child_exit() {
  local pid="$1"
  local attempt
  for ((attempt = 0; attempt < shutdown_poll_attempts; attempt++)); do
    if reap_if_exited "$pid"; then
      return 0
    fi
    sleep "$shutdown_poll_interval"
  done
  return 1
}

shutdown_child() {
  local pid="${1:-}"
  local initial_signal="${2:-INT}"
  [[ -n "$pid" ]] || return 0

  if reap_if_exited "$pid"; then
    return "$reaped_status"
  fi
  kill "-$initial_signal" "$pid" 2>/dev/null || true
  if ! wait_for_child_exit "$pid"; then
    kill -TERM "$pid" 2>/dev/null || true
    if wait_for_child_exit "$pid"; then
      echo "child $pid required TERM escalation" >&2
      return "$shutdown_term_escalation_status"
    fi
    kill -KILL "$pid" 2>/dev/null || true
    if wait_for_child_exit "$pid"; then
      echo "child $pid required KILL escalation" >&2
      return "$shutdown_kill_escalation_status"
    fi
    echo "child $pid survived the final shutdown deadline" >&2
    return "$shutdown_survivor_status"
  fi
  return "$reaped_status"
}

validate_command_history() {
  python3 - "$1" <<'PY'
import json
import sys

commands = json.load(open(sys.argv[1], encoding="utf-8"))
expected = [2, 66, 2, 0]
assert commands == expected, commands
PY
}

shutdown_node() {
  local node_status
  [[ -n "${node_pid:-}" ]] || return 0
  if shutdown_child "$node_pid" INT; then
    node_status=0
  else
    node_status=$?
  fi
  if [[ "$node_status" -ne "$shutdown_survivor_status" ]]; then
    node_pid=""
  fi
  return "$node_status"
}

shutdown_sensor() {
  local sensor_status
  [[ -n "${sensor_pid:-}" ]] || return 0
  if shutdown_child "$sensor_pid" TERM; then
    sensor_status=0
  else
    sensor_status=$?
  fi
  if [[ "$sensor_status" -ne "$shutdown_survivor_status" ]]; then
    sensor_pid=""
  fi
  return "$sensor_status"
}

cleanup() {
  local status=$?
  local cleanup_failed=0
  trap - EXIT
  if ! shutdown_child "${node_pid:-}" INT; then
    cleanup_failed=1
  fi
  if ! shutdown_child "${sensor_pid:-}" INT; then
    cleanup_failed=1
  fi
  if ! shutdown_child "${roscore_pid:-}" INT; then
    cleanup_failed=1
  fi
  if [[ -n "${temp_root:-}" ]]; then
    if ! rm -rf -- "$temp_root"; then
      cleanup_failed=1
    fi
  fi
  if (( cleanup_failed != 0 )); then
    echo "ROS 1 smoke cleanup did not stop every managed process" >&2
    if (( status == 0 )); then
      status=1
    fi
  fi
  exit "$status"
}

main() {
  repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
  temp_root="$(mktemp -d)"
  roscore_pid=""
  sensor_pid=""
  node_pid=""
  trap cleanup EXIT

  mkdir -p "$temp_root/ws/src"
  ln -s "$repo_root" "$temp_root/ws/src/netft_driver"
  cd "$temp_root/ws"
  catkin_make -DCMAKE_BUILD_TYPE=Release install >/dev/null
  set +u
  source install/setup.bash
  set -u

  expected_package_path="$temp_root/ws/install/share/netft_driver"
  resolved_package_path="$(rospack find netft_driver)"
  if [[ "$resolved_package_path" != "$expected_package_path" ]]; then
    echo "ROS 1 package resolved outside the temporary install space" >&2
    echo "expected: $expected_package_path" >&2
    echo "resolved: $resolved_package_path" >&2
    return 1
  fi
  echo "ROS 1 installed package path: $resolved_package_path"

  installed_module_path="$(
    python3 - "$temp_root/ws/install" <<'PY'
from pathlib import Path
import sys

import netft_driver


install_root = Path(sys.argv[1]).resolve()
module_path = Path(netft_driver.__file__).resolve()
try:
    module_path.relative_to(install_root)
except ValueError:
    raise SystemExit(
        "netft_driver imported outside the temporary install space: {}".format(
            module_path
        )
    )
print(module_path)
PY
  )"
  echo "ROS 1 installed Python module: $installed_module_path"

  master_port="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"
  export ROS_MASTER_URI="http://127.0.0.1:${master_port}"
  export ROS_HOSTNAME=127.0.0.1
  roscore -p "$master_port" >"$temp_root/roscore.log" 2>&1 &
  roscore_pid=$!
  for _ in $(seq 1 100); do
    timeout --kill-after=0.2s 0.5s rosparam list >/dev/null 2>&1 && break
    sleep 0.05
  done
  timeout --kill-after=0.2s 0.5s rosparam list >/dev/null

  PYTHONPATH="$repo_root" python3 "$repo_root/test/integration/fake_sensor_process.py" \
    --port-file "$temp_root/sensor.port" \
    --commands-file "$temp_root/commands.json" \
    --rate 200 >"$temp_root/sensor.log" 2>&1 &
  sensor_pid=$!
  for _ in $(seq 1 100); do
    [[ -s "$temp_root/sensor.port" ]] && break
    sleep 0.05
  done
  sensor_port="$(cat "$temp_root/sensor.port")"

  rosrun netft_driver netft_node \
    _sensor_ip:=127.0.0.1 \
    _sensor_port:="$sensor_port" \
    _expected_rdt_rate:=200.0 \
    _diagnostics_rate:=10.0 \
    >"$temp_root/node.log" 2>&1 &
  node_pid=$!

  for _ in $(seq 1 100); do
    timeout --kill-after=0.2s 0.5s rosservice info /netft/bias \
      >/dev/null 2>&1 && break
    sleep 0.05
  done
  timeout --kill-after=0.2s 0.5s rosservice info /netft/bias >/dev/null

  timeout --kill-after=2s 10s rostopic echo -n 1 /netft/wrench \
    >"$temp_root/wrench.txt"
  grep -q 'frame_id: "netft_link"' "$temp_root/wrench.txt"
  grep -q 'x: 0.0001' "$temp_root/wrench.txt"

  timeout --kill-after=2s 10s rostopic echo -n 1 /diagnostics \
    >"$temp_root/diagnostics.txt"
  grep -q 'netft_driver: connection' "$temp_root/diagnostics.txt"
  grep -q '127.0.0.1:' "$temp_root/diagnostics.txt"

  timeout --kill-after=2s 10s rosservice call /netft/bias '{}' \
    >"$temp_root/bias.txt"
  grep -q 'success: True' "$temp_root/bias.txt"
  timeout --kill-after=2s 10s rostopic echo -n 1 /netft/wrench \
    >"$temp_root/post_bias.txt"

  if shutdown_node; then
    node_status=0
  else
    node_status=$?
  fi
  if [[ "$node_status" -ne 0 ]]; then
    cat "$temp_root/node.log" >&2
    echo "ROS 1 node exited with status $node_status during normal shutdown" >&2
    return 1
  fi
  echo "ROS 1 node shutdown exit status: $node_status"
  if shutdown_sensor; then
    :
  else
    sensor_status=$?
    cat "$temp_root/sensor.log" >&2
    echo "ROS 1 fake sensor exited with status $sensor_status during normal shutdown" >&2
    return 1
  fi

  validate_command_history "$temp_root/commands.json"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  main "$@"
fi
