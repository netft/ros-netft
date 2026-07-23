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

  installed_node_path="$temp_root/ws/install/lib/netft_driver/netft_node"
  installed_check_path="$temp_root/ws/install/lib/netft_driver/netft_check"
  test -x "$installed_node_path"
  test -x "$installed_check_path"
  test "$(head -c 2 "$installed_node_path")" != '#!'
  rosrun netft_driver netft_check --help >/dev/null
  echo "ROS 1 installed native executable: $installed_node_path"

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
    --http-port-file "$temp_root/http.port" \
    --commands-file "$temp_root/commands.json" \
    --rate 200 >"$temp_root/sensor.log" 2>&1 &
  sensor_pid=$!
  for _ in $(seq 1 100); do
    [[ -s "$temp_root/sensor.port" && -s "$temp_root/http.port" ]] && break
    sleep 0.05
  done
  if [[ ! -s "$temp_root/sensor.port" || ! -s "$temp_root/http.port" ]]; then
    cat "$temp_root/sensor.log" >&2
    return 1
  fi
  sensor_port="$(cat "$temp_root/sensor.port")"
  http_port="$(cat "$temp_root/http.port")"

  rosrun netft_driver netft_node __ns:=/netft_smoke wrench:=remapped_wrench \
    _sensor_ip:=127.0.0.1 \
    _sensor_port:="$sensor_port" \
    _http_port:="$http_port" \
    _frame_id:=smoke_frame _wrench_topic:=wrench _bias_service:=bias \
    _use_sensor_calibration:=true _publish_rate:=50.0 \
    _receive_timeout:=0.8 _configuration_connect_timeout:=0.23 \
    _configuration_timeout:=0.71 \
    _reconnect_initial_delay:=0.11 _reconnect_max_delay:=0.37 \
    _expected_rdt_rate:=200.0 \
    _diagnostics_rate:=5.0 _rate_tolerance:=0.35 _publish_on_error:=true \
    >"$temp_root/node.log" 2>&1 &
  node_pid=$!

  for _ in $(seq 1 100); do
    timeout --kill-after=0.2s 0.5s rosservice info /netft_smoke/bias \
      >/dev/null 2>&1 && break
    sleep 0.05
  done
  timeout --kill-after=0.2s 0.5s rosservice info /netft_smoke/bias >/dev/null
  test "$(timeout --kill-after=0.2s 0.5s rosservice type /netft_smoke/bias)" = \
    "std_srvs/Trigger"
  timeout --kill-after=1s 3s rosnode info /netft_smoke/netft \
    >"$temp_root/graph.txt"
  grep -q '^ \* /netft_smoke/remapped_wrench ' "$temp_root/graph.txt"
  grep -Fxq ' * /netft_smoke/bias' "$temp_root/graph.txt"
  timeout --kill-after=0.2s 0.5s rostopic list >"$temp_root/topics.txt"
  grep -Fxq '/netft_smoke/remapped_wrench' "$temp_root/topics.txt"
  if grep -Fxq '/netft_smoke/wrench' "$temp_root/topics.txt"; then
    echo "unremapped relative wrench topic unexpectedly exists" >&2
    return 1
  fi

  timeout --kill-after=2s 10s rostopic echo -n 1 /netft_smoke/remapped_wrench \
    >"$temp_root/wrench.txt"
  python3 "$repo_root/test/integration/ros_graph_assertions.py" wrench \
    "$temp_root/wrench.txt" --ros-version 1 --frame-id smoke_frame \
    --axes 100.0 -200.0 300.0 0.001 -0.002 0.003

  sleep 1
  timeout --kill-after=2s 10s rostopic echo -n 5 /diagnostics \
    >"$temp_root/diagnostics.txt"
  grep -q 'netft_driver: connection' "$temp_root/diagnostics.txt"
  grep -q "hardware_id: \"127.0.0.1:$sensor_port\"" "$temp_root/diagnostics.txt"
  python3 "$repo_root/test/integration/ros_graph_assertions.py" diagnostics \
    "$temp_root/diagnostics.txt" --ros-version 1 \
    --status-header "$repo_root/src/ros/diagnostics.hpp" \
    --expected-rate 5.0 --configured-publish-rate 50.0 \
    --expected-value "sensor=127.0.0.1:$sensor_port" \
    --expected-value expected_receive_rate_hz=200.0 \
    --expected-value rate_tolerance=0.350 \
    --expected-value configuration_source=sensor \
    --expected-value force_unit=kN \
    --expected-value torque_unit=N-mm \
    --expected-value counts_per_force_unit=1000 \
    --expected-value counts_per_torque_unit=10

  timeout --kill-after=2s 10s rosservice call /netft_smoke/bias '{}' \
    >"$temp_root/bias.txt"
  grep -q 'success: True' "$temp_root/bias.txt"
  timeout --kill-after=2s 10s rostopic echo -n 1 /netft_smoke/remapped_wrench \
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
