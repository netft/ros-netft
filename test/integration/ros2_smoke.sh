#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
source "$repo_root/test/integration/ros2_domain_lease.sh"

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
  local domain
  for domain in "${NETFT_ROS2_LEASED_DOMAINS[@]}"; do
    if ! stop_ros2_daemon_for_domain "$domain"; then
      cleanup_failed=1
    fi
  done
  if ! release_ros2_domain_leases; then
    cleanup_failed=1
  fi
  if [[ -n "${temp_root:-}" ]]; then
    if ! rm -rf -- "$temp_root"; then
      cleanup_failed=1
    fi
  fi
  if (( cleanup_failed != 0 )); then
    echo "ROS 2 smoke cleanup failed" >&2
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
    --cmake-args -DNETFT_INSTALL_TEST_TOOLS=ON \
    --event-handlers console_direct+ >/dev/null
  set +u
  source install/setup.bash
  set -u
  ros2 pkg executables netft_driver >"$temp_root/executables.txt"
  grep -Fxq 'netft_driver netft_node' "$temp_root/executables.txt"
  grep -Fxq 'netft_driver netft_check' "$temp_root/executables.txt"
  test -x "$temp_root/ws/install/netft_driver/lib/netft_driver/netft_node"
  test -x "$temp_root/ws/install/netft_driver/lib/netft_driver/netft_check"
  installed_probe="$temp_root/ws/install/netft_driver/share/netft_driver/test/integration/ros2_graph_probe.py"
  test -x "$installed_probe"
  ! find "$temp_root/ws/install/netft_driver" -type d -path '*/site-packages/netft_driver' -print -quit | grep -q .
  ros2 run netft_driver netft_check --help >/dev/null
}

run_full_graph_scenario() {
  local scenario_root="$1"
  local domain="$2"
  local sensor_port
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

  setsid ros2 run netft_driver netft_node --ros-args -r __ns:=/netft_smoke -r wrench:=remapped_wrench \
    -p sensor_ip:=127.0.0.1 \
    -p sensor_port:="$sensor_port" \
    -p frame_id:=smoke_frame -p wrench_topic:=wrench -p bias_service:=bias \
    -p counts_per_force:=2000000.0 -p counts_per_torque:=4000000.0 -p publish_rate:=50.0 \
    -p expected_rdt_rate:=200.0 \
    -p receive_timeout:=0.8 -p reconnect_initial_delay:=0.11 -p reconnect_max_delay:=0.37 \
    -p diagnostics_rate:=5.0 -p rate_tolerance:=0.35 -p publish_on_error:=true \
    >"$scenario_root/node.log" 2>&1 &
  node_pid=$!

  if ! timeout --kill-after=2s 20s python3 "$installed_probe" full \
    --node-name /netft_smoke/netft \
    --service-name /netft_smoke/bias \
    --wrench-topic /netft_smoke/remapped_wrench \
    --absent-topic /netft_smoke/wrench \
    --diagnostics-topic /diagnostics \
    --wrench-output "$scenario_root/wrench.txt" \
    --diagnostics-output "$scenario_root/diagnostics.txt" \
    --post-bias-output "$scenario_root/post_bias.txt" \
    --bias-output "$scenario_root/bias.txt" \
    --timeout 15 >"$scenario_root/probe.log" 2>&1; then
    cat "$scenario_root/probe.log" >&2
    cat "$scenario_root/node.log" >&2
    return 1
  fi
  if ! kill -0 "$node_pid" 2>/dev/null; then
    cat "$scenario_root/node.log" >&2
    return 1
  fi

  python3 "$repo_root/test/integration/ros_graph_assertions.py" wrench \
    "$scenario_root/wrench.txt" --ros-version 2 --frame-id smoke_frame \
    --axes 0.00005 -0.0001 0.00015 0.0000025 -0.000005 0.0000075
  python3 "$repo_root/test/integration/ros_graph_assertions.py" wrench \
    "$scenario_root/post_bias.txt" --ros-version 2 --frame-id smoke_frame \
    --axes 0.0 0.0 0.0 0.0 0.0 0.0

  assert_file_contains 'netft_driver: connection' \
    "$scenario_root/diagnostics.txt"
  assert_file_contains "hardware_id: \"127.0.0.1:$sensor_port\"" \
    "$scenario_root/diagnostics.txt"
  python3 "$repo_root/test/integration/ros_graph_assertions.py" diagnostics \
    "$scenario_root/diagnostics.txt" --ros-version 2 \
    --status-header "$repo_root/include/netft_driver/status.hpp" \
    --expected-rate 5.0 --configured-publish-rate 50.0 \
    --expected-value "sensor=127.0.0.1:$sensor_port" \
    --expected-value expected_receive_rate_hz=200.0 \
    --expected-value rate_tolerance=0.350

  assert_file_contains 'success=True' "$scenario_root/bias.txt"

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
  validate_command_history "$scenario_root/commands.json" '[2, 66, 2, 0]'
  echo "ROS 2 full graph scenario passed: exit 0, commands [2, 66, 2, 0]"
}

run_shutdown_scenario() {
  local scenario_root="$1"
  local domain="$2"
  local sensor_port
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

  if ! timeout --kill-after=2s 20s python3 "$installed_probe" ready \
    --node-name /netft \
    --service-name /netft/bias \
    --wrench-topic /netft/wrench \
    --wrench-output "$scenario_root/wrench.txt" \
    --timeout 15 >"$scenario_root/probe.log" 2>&1; then
    cat "$scenario_root/probe.log" >&2
    cat "$scenario_root/node.log" >&2
    return 1
  fi
  python3 "$repo_root/test/integration/ros_graph_assertions.py" wrench \
    "$scenario_root/wrench.txt" --ros-version 2 --frame-id netft_link \
    --axes 0.0001 -0.0002 0.0003 0.00001 -0.00002 0.00003

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
  full_domain=""
  shutdown_domain=""
  export ROS_LOCALHOST_ONLY=1
  temp_root=""
  sensor_pid=""
  node_pid=""
  trap cleanup EXIT

  acquire_ros2_domain full_domain
  acquire_ros2_domain shutdown_domain
  export ROS_DOMAIN_ID="$full_domain"
  temp_root="$(mktemp -d)"

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
