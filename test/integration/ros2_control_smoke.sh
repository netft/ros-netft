#!/usr/bin/env bash
set -euo pipefail

installed_share=""
smoke_root=""
managed_pids=()

run_source_smoke() {
  local source_root
  local build_root
  local installed_entry
  source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
  build_root="$(mktemp -d)"
  trap 'rm -rf -- "$build_root"' EXIT
  mkdir -p "$build_root/ws/src"
  ln -s "$source_root" "$build_root/ws/src/netft_driver"
  (
    cd "$build_root/ws"
    colcon build --base-paths src --packages-select netft_driver \
      --cmake-args -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
      -DNETFT_INSTALL_TEST_TOOLS=ON
  )
  set +u
  source "$build_root/ws/install/setup.bash"
  set -u
  installed_entry="$(ros2 pkg prefix --share netft_driver)/test/integration/ros2_control_smoke.sh"
  export NETFT_ROS2_CONTROL_SMOKE_ROOT="$build_root"
  trap - EXIT
  echo "executing installed smoke entry: $installed_entry"
  exec "$installed_entry" --installed
}

terminate_pid() {
  local pid="$1"
  kill -INT "$pid" 2>/dev/null || true
  local deadline=$((SECONDS + 3))
  while kill -0 "$pid" 2>/dev/null && (( SECONDS < deadline )); do
    sleep 0.05
  done
  if kill -0 "$pid" 2>/dev/null; then
    kill -KILL "$pid" 2>/dev/null || true
  fi
  wait "$pid" 2>/dev/null || true
}

render_control_logs() {
  local log
  for log in robot_state_publisher.log controller_manager.log; do
    echo "$log:"
    if [[ -f "$smoke_root/$log" ]]; then
      cat "$smoke_root/$log"
    else
      echo "not created"
    fi
  done
}

cleanup() {
  local status=$?
  local cleanup_failed=0
  local diagnostics
  local domain
  local pid
  trap - EXIT INT TERM
  for pid in "${managed_pids[@]}"; do
    terminate_pid "$pid"
  done
  diagnostics="$(render_control_logs)"
  for domain in "${NETFT_ROS2_LEASED_DOMAINS[@]}"; do
    if ! stop_ros2_daemon_for_domain "$domain"; then
      cleanup_failed=1
    fi
  done
  if ! release_ros2_domain_leases; then
    echo "failed to release ROS 2 domain leases" >&2
    cleanup_failed=1
  fi
  if ! rm -rf -- "$smoke_root"; then
    echo "failed to remove ROS 2 control smoke root: $smoke_root" >&2
    cleanup_failed=1
  fi
  if ((cleanup_failed != 0)); then
    echo "ROS 2 control smoke cleanup failed" >&2
    if ((status == 0)); then
      status=1
    fi
  fi
  if ((status != 0)); then
    printf '%s\n' "$diagnostics" >&2
  fi
  exit "$status"
}

run_installed_smoke() {
  local script_path
  local domain=""
  local installed_probe
  local left_http_port
  local left_port
  local manager_executable
  local manager_pid
  local right_http_port
  local right_port
  local robot_description
  local rsp_executable
  script_path="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)/$(basename "${BASH_SOURCE[0]}")"
  installed_share="$(cd "$(dirname "$script_path")/../.." && pwd -P)"
  case "$script_path" in
    */share/netft_driver/test/integration/ros2_control_smoke.sh) ;;
    *) echo "installed smoke must run from share/netft_driver" >&2; return 2 ;;
  esac
  smoke_root="${NETFT_ROS2_CONTROL_SMOKE_ROOT:-$(mktemp -d)}"
  source "$installed_share/test/integration/ros2_domain_lease.sh"
  managed_pids=()
  trap cleanup EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM

  acquire_ros2_domain domain
  export ROS_DOMAIN_ID="$domain"
  export ROS_LOCALHOST_ONLY=1
  echo "netft_control_domain=$domain"

  start_fake() {
    local side="$1"
    PYTHONPATH="$installed_share${PYTHONPATH:+:$PYTHONPATH}" \
      python3 "$installed_share/test/integration/fake_sensor_process.py" \
      --port-file "$smoke_root/$side.port" \
      --http-port-file "$smoke_root/$side.http_port" \
      --commands-file "$smoke_root/$side.commands" --rate 200 &
    managed_pids+=("$!")
    local deadline=$((SECONDS + 10))
    while [[ ! -s "$smoke_root/$side.port" || ! -s "$smoke_root/$side.http_port" ]]; do
      if (( SECONDS >= deadline )); then
        echo "fake sensor $side did not start" >&2
        return 1
      fi
      sleep 0.05
    done
  }

  start_fake left
  start_fake right
  left_http_port="$(<"$smoke_root/left.http_port")"
  left_port="$(<"$smoke_root/left.port")"
  right_http_port="$(<"$smoke_root/right.http_port")"
  right_port="$(<"$smoke_root/right.port")"
  test "$installed_share" = "$(ros2 pkg prefix --share netft_driver)"
  installed_probe="$installed_share/test/integration/ros2_graph_probe.py"
  test -x "$installed_probe"

  cat >"$smoke_root/dual.urdf.xacro" <<EOF
<?xml version="1.0"?>
<robot name="dual_netft" xmlns:xacro="http://www.ros.org/wiki/xacro">
  <link name="base"/>
  <xacro:include filename="$installed_share/urdf/netft.ros2_control.xacro"/>
  <xacro:netft_ros2_control name="left_hardware" sensor_name="left_ft"
    sensor_ip="127.0.0.1" sensor_port="$left_port" http_port="$left_http_port"
    use_sensor_calibration="true" counts_per_force="0" counts_per_torque="nan"
    receive_timeout="5.0" configuration_connect_timeout="0.25"
    configuration_timeout="0.75" activation_timeout="2.0"/>
  <xacro:netft_ros2_control name="right_hardware" sensor_name="right_ft"
    sensor_ip="127.0.0.1" sensor_port="$right_port" http_port="$right_http_port"
    use_sensor_calibration="true" counts_per_force="0" counts_per_torque="nan"
    receive_timeout="5.0" configuration_connect_timeout="0.25"
    configuration_timeout="0.75" activation_timeout="2.0"/>
</robot>
EOF

  cat >"$smoke_root/controllers.yaml" <<'EOF'
controller_manager:
  ros__parameters:
    update_rate: 200
    left_broadcaster:
      type: force_torque_sensor_broadcaster/ForceTorqueSensorBroadcaster
    right_broadcaster:
      type: force_torque_sensor_broadcaster/ForceTorqueSensorBroadcaster
left_broadcaster:
  ros__parameters:
    sensor_name: left_ft
    frame_id: left_ft
right_broadcaster:
  ros__parameters:
    sensor_name: right_ft
    frame_id: right_ft
EOF

  robot_description="$(timeout --kill-after=2s 10s xacro "$smoke_root/dual.urdf.xacro")"
  rsp_executable="$(ros2 pkg prefix robot_state_publisher)/lib/robot_state_publisher/robot_state_publisher"
  "$rsp_executable" --ros-args -p "robot_description:=$robot_description" \
    >"$smoke_root/robot_state_publisher.log" 2>&1 &
  managed_pids+=("$!")
  manager_executable="$(ros2 pkg prefix controller_manager)/lib/controller_manager/ros2_control_node"
  "$manager_executable" \
    --ros-args --params-file "$smoke_root/controllers.yaml" \
    --remap "~/robot_description:=/robot_description" \
    >"$smoke_root/controller_manager.log" 2>&1 &
  manager_pid="$!"
  managed_pids+=("$manager_pid")

  dump_hardware_components() {
    timeout --kill-after=2s 5s ros2 control list_hardware_components
  }

  spawn_controller() {
    local controller="$1"
    if ! timeout --kill-after=2s 15s ros2 run controller_manager spawner "$controller" \
        --controller-manager /controller_manager \
        --param-file "$smoke_root/controllers.yaml"; then
      echo "failed to spawn $controller" >&2
      dump_hardware_components >&2 || true
      return 1
    fi
  }

  run_control_probe() {
    if ! timeout --kill-after=2s 20s python3 "$installed_probe" \
        "$@" --timeout 15 >"$smoke_root/probe.log" 2>&1; then
      cat "$smoke_root/probe.log" >&2
      dump_hardware_components >&2 || true
      return 1
    fi
  }

  spawn_controller left_broadcaster
  spawn_controller right_broadcaster
  run_control_probe control \
    --service-name /left_ft/bias \
    --service-name /right_ft/bias \
    --wrench-topic /left_broadcaster/wrench \
    --wrench-topic /right_broadcaster/wrench \
    --bias-service /right_ft/bias \
    --bias-wrench-topic /right_broadcaster/wrench
  echo "netft_control_event=publish_ready"

  terminate_pid "${managed_pids[0]}"

  component_state() {
    local component="$1"
    timeout --kill-after=2s 5s ros2 control list_hardware_components 2>/dev/null | \
      sed -E $'s/\x1B\[[0-9;]*[mK]//g' | \
      awk -v component="$component" '
        $1 == "name:" && $2 == component { found = 1; next }
        found && $1 == "state:" { state = $NF; sub(/^label=/, "", state); print state; exit }
      '
  }
  local deadline=$((SECONDS + 15))
  local left_state
  local right_state
  while true; do
    left_state="$(component_state left_hardware)"
    right_state="$(component_state right_hardware)"
    if [[ -n "$left_state" && "$left_state" != "active" && "$right_state" == "active" ]]; then
      break
    fi
    if (( SECONDS >= deadline )); then
      echo "unexpected post-fault states: left=$left_state right=$right_state" >&2
      dump_hardware_components >&2 || true
      return 1
    fi
    sleep 0.1
  done
  echo "netft_control_state=left:$left_state,right:$right_state"
  kill -0 "$manager_pid"

  run_control_probe control \
    --service-name /right_ft/bias \
    --wrench-topic /right_broadcaster/wrench
  echo "netft_control_event=right_survived"
  echo "netft_control_result=pass"
}

main() {
  if [[ "${1:-}" == "--installed" ]]; then
    run_installed_smoke
  else
    run_source_smoke
  fi
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  main "$@"
fi
