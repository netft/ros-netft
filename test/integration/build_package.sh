#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
temp_root="$(mktemp -d)"
roscore_pid=""
cleanup() {
  if [[ -n "$roscore_pid" ]]; then
    kill "$roscore_pid" 2>/dev/null || true
    wait "$roscore_pid" 2>/dev/null || true
  fi
  rm -rf -- "$temp_root"
}
trap cleanup EXIT

mkdir -p "$temp_root/ws/src"
ln -s "$repo_root" "$temp_root/ws/src/netft_driver"

assert_native_install() {
  local install_root="$1"
  local ros_version="$2"
  local share_root
  if [[ "$ros_version" == "1" ]]; then
    share_root="$install_root/share/netft_driver"
    test -f "$share_root/launch/netft.launch"
    test -f "$share_root/config/netft_ros1.yaml"
    test ! -e "$share_root/launch/netft.launch.py"
    test ! -e "$share_root/config/netft_ros2.yaml"
    test -x "$install_root/lib/netft_driver/netft_node"
    test -x "$install_root/lib/netft_driver/netft_check"
    test ! -e "$install_root/lib/python3/dist-packages/netft_driver"
  else
    share_root="$install_root/share/netft_driver"
    test -f "$share_root/launch/netft.launch.py"
    test -f "$share_root/launch/netft_ros2_control.launch.py"
    test -f "$share_root/config/netft_ros2.yaml"
    test -f "$share_root/config/netft_ros2_control.yaml"
    test -f "$share_root/urdf/netft.ros2_control.xacro"
    test ! -e "$share_root/test"
    test ! -e "$share_root/launch/netft.launch"
    test ! -e "$share_root/config/netft_ros1.yaml"
    test -x "$install_root/lib/netft_driver/netft_node"
    test -x "$install_root/lib/netft_driver/netft_check"
    ! find "$install_root" -type d -path '*/site-packages/netft_driver' -print -quit | grep -q .
  fi
}

validate_junit_result() {
  python3 "$repo_root/test/integration/validate_junit.py" \
    "$temp_root/ws/build" "$1" "$2"
}

if [[ "${ROS_VERSION:-}" == "1" ]]; then
  cd "$temp_root/ws"
  catkin_make -DCMAKE_BUILD_TYPE=Release install
  assert_native_install "$temp_root/ws/install" 1
  master_port="$(python3 -c 'import socket; sock = socket.socket(); sock.bind(("127.0.0.1", 0)); print(sock.getsockname()[1]); sock.close()')"
  export ROS_MASTER_URI="http://127.0.0.1:${master_port}"
  export ROS_HOSTNAME="127.0.0.1"
  roscore -p "$master_port" >"$temp_root/roscore.log" 2>&1 &
  roscore_pid="$!"
  for _ in $(seq 1 100); do
    if rosparam list >/dev/null 2>&1; then
      break
    fi
    if ! kill -0 "$roscore_pid" 2>/dev/null; then
      cat "$temp_root/roscore.log" >&2
      exit 1
    fi
    sleep 0.1
  done
  rosparam list >/dev/null
  catkin_make run_tests
  catkin_test_results --all build/test_results
  validate_junit_result netft_unit at-least-one
  validate_junit_result netft_ros1_smoke_harness exactly-one
  validate_junit_result netft_ros1_node at-least-one
elif [[ "${ROS_VERSION:-}" == "2" ]]; then
  cd "$temp_root/ws"
  colcon build \
    --base-paths src \
    --packages-select netft_driver \
    --event-handlers console_direct+ \
    --cmake-args -DCMAKE_BUILD_TYPE=Release
  assert_native_install \
    "$temp_root/ws/install/netft_driver" 2
  colcon test \
    --packages-select netft_driver \
    --event-handlers console_direct+
  colcon test-result --verbose
  ctest --test-dir build/netft_driver -N >build/netft_driver/registered-tests.txt
  grep -q 'netft_unit' build/netft_driver/registered-tests.txt
  grep -q 'netft_ros2_smoke_harness' \
    build/netft_driver/registered-tests.txt
  grep -q 'netft_ros2_domain_lease_harness' \
    build/netft_driver/registered-tests.txt
  grep -q 'netft_ros2_control_cleanup_harness' \
    build/netft_driver/registered-tests.txt
  validate_junit_result netft_unit at-least-one
  validate_junit_result netft_ros2_smoke_harness exactly-one
  validate_junit_result netft_ros2_domain_lease_harness exactly-one
  validate_junit_result netft_ros2_control_cleanup_harness exactly-one
  validate_junit_result netft_ros2_node at-least-one
  validate_junit_result netft_hardware_interface at-least-one
else
  echo "ROS_VERSION must be 1 or 2" >&2
  exit 2
fi
