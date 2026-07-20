#!/usr/bin/env bash
set -euo pipefail

shutdown_poll_attempts=20
shutdown_poll_interval=0.05
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
    if ! wait_for_child_exit "$pid"; then
      kill -KILL "$pid" 2>/dev/null || true
      if ! wait_for_child_exit "$pid"; then
        echo "child $pid survived the final shutdown deadline" >&2
        return 1
      fi
    fi
  fi
  return "$reaped_status"
}

process_group_exited_and_reaped() {
  local group_id="$1"
  local leader_reaped=0
  if reap_if_exited "$group_id"; then
    leader_reaped=1
  fi
  if kill -0 -- "-$group_id" 2>/dev/null; then
    return 1
  fi
  (( leader_reaped != 0 ))
}

wait_for_process_group_exit() {
  local group_id="$1"
  local attempt
  for ((attempt = 0; attempt < shutdown_poll_attempts; attempt++)); do
    if process_group_exited_and_reaped "$group_id"; then
      return 0
    fi
    sleep "$shutdown_poll_interval"
  done
  return 1
}

wait_for_process_group_start() {
  local group_id="$1"
  local attempt
  for ((attempt = 0; attempt < shutdown_poll_attempts; attempt++)); do
    if kill -0 -- "-$group_id" 2>/dev/null; then
      return 0
    fi
    if ! kill -0 "$group_id" 2>/dev/null; then
      return 1
    fi
    sleep "$shutdown_poll_interval"
  done
  return 1
}

shutdown_process_group() {
  local group_id="${1:-}"
  local initial_signal="${2:-INT}"
  [[ -n "$group_id" ]] || return 0

  if wait_for_process_group_start "$group_id"; then
    kill "-$initial_signal" -- "-$group_id" 2>/dev/null || true
    if wait_for_process_group_exit "$group_id"; then
      return "$reaped_status"
    fi
    kill -TERM -- "-$group_id" 2>/dev/null || true
    if wait_for_process_group_exit "$group_id"; then
      return 1
    fi
    kill -KILL -- "-$group_id" 2>/dev/null || true
    if ! wait_for_process_group_exit "$group_id"; then
      echo "process group $group_id survived the final shutdown deadline" >&2
      return 1
    fi
    return 1
  else
    shutdown_child "$group_id" "$initial_signal"
    return
  fi
}

stop_ros2_daemon() {
  timeout --kill-after=2s 5s ros2 daemon stop >/dev/null 2>&1 || true
}

validate_command_history() {
  python3 - "$1" "$2" <<'PY'
import json
import sys

commands = json.load(open(sys.argv[1], encoding="utf-8"))
expected = json.loads(sys.argv[2])
assert commands == expected, commands
PY
}

assert_file_contains() {
  local pattern="$1"
  local path="$2"
  if ! grep -q "$pattern" "$path"; then
    cat "$path" >&2
    return 1
  fi
}

cleanup() {
  local status=$?
  local cleanup_failed=0
  trap - EXIT
  if ! shutdown_process_group "${node_pid:-}" INT; then
    cleanup_failed=1
  fi
  if ! shutdown_child "${sensor_pid:-}" INT; then
    cleanup_failed=1
  fi
  stop_ros2_daemon
  if [[ -n "${temp_root:-}" ]]; then
    if ! rm -rf -- "$temp_root"; then
      cleanup_failed=1
    fi
  fi
  if (( cleanup_failed != 0 )); then
    echo "ROS 2 smoke cleanup did not stop every managed process" >&2
    if (( status == 0 )); then
      status=1
    fi
  fi
  exit "$status"
}

prepare_workspace() {
  mkdir -p "$temp_root/ws/src"
  ln -s "$repo_root" "$temp_root/ws/src/netft_driver"
  cd "$temp_root/ws"
  timeout --kill-after=30s 180s colcon build \
    --base-paths src \
    --packages-select netft_driver \
    --event-handlers console_direct+ >/dev/null
  set +u
  source install/setup.bash
  set -u
}

run_full_graph_scenario() {
  local scenario_root="$1"
  local domain="$2"
  local sensor_port
  local service_type
  local group_id
  local node_status

  mkdir -p "$scenario_root"
  export ROS_DOMAIN_ID="$domain"

  PYTHONPATH="$repo_root" python3 \
    "$repo_root/test/integration/fake_sensor_process.py" \
    --port-file "$scenario_root/sensor.port" \
    --commands-file "$scenario_root/commands.json" \
    --rate 200 >"$scenario_root/sensor.log" 2>&1 &
  sensor_pid=$!
  for _ in $(seq 1 100); do
    [[ -s "$scenario_root/sensor.port" ]] && break
    kill -0 "$sensor_pid" 2>/dev/null || break
    sleep 0.05
  done
  if [[ ! -s "$scenario_root/sensor.port" ]]; then
    cat "$scenario_root/sensor.log" >&2
    return 1
  fi
  sensor_port="$(<"$scenario_root/sensor.port")"

  setsid ros2 run netft_driver netft_node --ros-args \
    -p sensor_ip:=127.0.0.1 \
    -p sensor_port:="$sensor_port" \
    -p expected_rdt_rate:=200.0 \
    -p receive_timeout:=1.0 \
    -p diagnostics_rate:=10.0 \
    >"$scenario_root/node.log" 2>&1 &
  node_pid=$!

  for _ in $(seq 1 100); do
    if timeout --kill-after=1s 3s ros2 service type /netft/bias \
      >/dev/null 2>&1; then
      break
    fi
    kill -0 "$node_pid" 2>/dev/null || break
    sleep 0.05
  done
  if ! kill -0 "$node_pid" 2>/dev/null; then
    cat "$scenario_root/node.log" >&2
    return 1
  fi
  service_type="$(
    timeout --kill-after=1s 3s ros2 service type /netft/bias
  )"
  test "$service_type" = "std_srvs/srv/Trigger"

  timeout --kill-after=2s 10s ros2 topic echo --once /netft/wrench \
    >"$scenario_root/wrench.txt"
  assert_file_contains 'frame_id: netft_link' "$scenario_root/wrench.txt"
  assert_file_contains 'x: 0.0001' "$scenario_root/wrench.txt"

  timeout --kill-after=2s 10s ros2 topic echo --once /diagnostics \
    >"$scenario_root/diagnostics.txt"
  assert_file_contains 'netft_driver: connection' \
    "$scenario_root/diagnostics.txt"
  assert_file_contains 'hardware_id: 127.0.0.1:' \
    "$scenario_root/diagnostics.txt"
  assert_file_contains 'level: "\\0"' "$scenario_root/diagnostics.txt"

  timeout --kill-after=2s 10s \
    ros2 service call /netft/bias std_srvs/srv/Trigger '{}' \
    >"$scenario_root/bias.txt"
  assert_file_contains 'success=True' "$scenario_root/bias.txt"
  timeout --kill-after=2s 10s ros2 topic echo --once /netft/wrench \
    >"$scenario_root/post_bias.txt"

  group_id="$node_pid"
  set +e
  shutdown_process_group "$group_id" INT
  node_status=$?
  set -e
  if process_group_exited_and_reaped "$group_id"; then
    node_pid=""
  else
    echo "ROS 2 full graph process group survived shutdown" >&2
    return 1
  fi
  if [[ "$node_status" -ne 0 ]]; then
    cat "$scenario_root/node.log" >&2
    echo "ROS 2 node exited with status $node_status during normal shutdown" >&2
    return 1
  fi
  if grep -q 'ExternalShutdownException' "$scenario_root/node.log"; then
    cat "$scenario_root/node.log" >&2
    echo "ROS 2 node emitted an external-shutdown traceback" >&2
    return 1
  fi

  shutdown_child "$sensor_pid" TERM
  sensor_pid=""
  stop_ros2_daemon
  validate_command_history "$scenario_root/commands.json" '[2, 66, 2, 0]'
  echo "ROS 2 full graph scenario passed: exit 0, commands [2, 66, 2, 0]"
}

run_shutdown_scenario() {
  local scenario_root="$1"
  local domain="$2"
  local sensor_port
  local service_ready=0
  local group_id
  local leader_stopped=0
  local group_stopped=0
  local node_status
  local state

  mkdir -p "$scenario_root"
  export ROS_DOMAIN_ID="$domain"

  PYTHONPATH="$repo_root" python3 \
    "$repo_root/test/integration/fake_sensor_process.py" \
    --port-file "$scenario_root/sensor.port" \
    --commands-file "$scenario_root/commands.json" \
    --rate 200 >"$scenario_root/sensor.log" 2>&1 &
  sensor_pid=$!
  for _ in $(seq 1 100); do
    [[ -s "$scenario_root/sensor.port" ]] && break
    kill -0 "$sensor_pid" 2>/dev/null || break
    sleep 0.05
  done
  if [[ ! -s "$scenario_root/sensor.port" ]]; then
    cat "$scenario_root/sensor.log" >&2
    return 1
  fi
  sensor_port="$(<"$scenario_root/sensor.port")"

  setsid ros2 run netft_driver netft_node --ros-args \
    -p sensor_ip:=127.0.0.1 \
    -p sensor_port:="$sensor_port" \
    -p expected_rdt_rate:=200.0 \
    -p receive_timeout:=1.0 \
    >"$scenario_root/node.log" 2>&1 &
  node_pid=$!

  for _ in $(seq 1 100); do
    if timeout --kill-after=1s 3s ros2 service type /netft/bias \
      >/dev/null 2>&1; then
      service_ready=1
      break
    fi
    kill -0 "$node_pid" 2>/dev/null || break
    sleep 0.05
  done
  if [[ "$service_ready" -ne 1 ]]; then
    cat "$scenario_root/node.log" >&2
    return 1
  fi
  timeout --kill-after=2s 10s ros2 topic echo --once /netft/wrench \
    >"$scenario_root/wrench.txt"
  assert_file_contains 'frame_id: netft_link' "$scenario_root/wrench.txt"

  group_id="$node_pid"
  kill -INT -- "-$group_id"
  for _ in $(seq 1 50); do
    if ! kill -0 "$node_pid" 2>/dev/null; then
      leader_stopped=1
      break
    fi
    state="$(process_state "$node_pid" 2>/dev/null || true)"
    if [[ "$state" == "Z" ]]; then
      leader_stopped=1
      break
    fi
    sleep 0.1
  done
  if [[ "$leader_stopped" -ne 1 ]]; then
    kill -TERM -- "-$group_id" 2>/dev/null || true
    shutdown_process_group "$group_id" KILL || true
    if process_group_exited_and_reaped "$group_id"; then
      node_pid=""
    fi
    echo "node leader did not stop inside the SIGINT deadline" >&2
    return 1
  fi

  set +e
  wait "$node_pid"
  node_status=$?
  set -e

  for _ in $(seq 1 20); do
    if ! kill -0 -- "-$group_id" 2>/dev/null; then
      group_stopped=1
      break
    fi
    sleep 0.05
  done
  if [[ "$group_stopped" -ne 1 ]]; then
    shutdown_process_group "$group_id" TERM || true
    if process_group_exited_and_reaped "$group_id"; then
      node_pid=""
    fi
    echo "node process group remained live after leader exit" >&2
    return 1
  fi
  node_pid=""

  shutdown_child "$sensor_pid" TERM
  sensor_pid=""
  stop_ros2_daemon
  validate_command_history "$scenario_root/commands.json" '[2, 0]'
  if grep -q 'ExternalShutdownException' "$scenario_root/node.log"; then
    cat "$scenario_root/node.log" >&2
    echo "normal SIGINT emitted ExternalShutdownException" >&2
    return 1
  fi
  if [[ "$node_status" -ne 0 ]]; then
    cat "$scenario_root/node.log" >&2
    echo "normal SIGINT returned node status $node_status, expected 0" >&2
    return 1
  fi
  echo "ROS 2 shutdown-only scenario passed: exit 0, commands [2, 0]"
}

main() {
  repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
  base_domain="$((100 + $$ % 40))"
  full_domain="$base_domain"
  shutdown_domain="$((base_domain + 50))"
  export ROS_DOMAIN_ID="$full_domain"
  export ROS_LOCALHOST_ONLY=1
  temp_root="$(mktemp -d)"
  sensor_pid=""
  node_pid=""
  trap cleanup EXIT

  prepare_workspace

  reaped_pid=""
  reaped_status=0
  run_full_graph_scenario "$temp_root/full" "$full_domain"

  reaped_pid=""
  reaped_status=0
  run_shutdown_scenario "$temp_root/shutdown" "$shutdown_domain"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  main "$@"
fi
