#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
temp_root="$(mktemp -d)"
cleanup() {
  rm -rf -- "$temp_root"
}
trap cleanup EXIT

mkdir -p "$temp_root/ws/src"
ln -s "$repo_root" "$temp_root/ws/src/netft_driver"

assert_native_install() {
  local share_root="$1"
  local ros_version="$2"
  if [[ "$ros_version" == "1" ]]; then
    test -f "$share_root/launch/netft.launch"
    test -f "$share_root/config/netft_ros1.yaml"
    test ! -e "$share_root/launch/netft.launch.py"
    test ! -e "$share_root/config/netft_ros2.yaml"
  else
    test -f "$share_root/launch/netft.launch.py"
    test -f "$share_root/config/netft_ros2.yaml"
    test ! -e "$share_root/launch/netft.launch"
    test ! -e "$share_root/config/netft_ros1.yaml"
  fi
}

validate_junit_result() {
  python3 "$repo_root/test/integration/validate_junit.py" \
    "$temp_root/ws/build" "$1" "$2"
}

if [[ "${ROS_VERSION:-}" == "1" ]]; then
  cd "$temp_root/ws"
  catkin_make -DCMAKE_BUILD_TYPE=Release install
  assert_native_install "$temp_root/ws/install/share/netft_driver" 1
  catkin_make run_tests
  catkin_test_results --all build/test_results
  validate_junit_result netft_unit at-least-one
  validate_junit_result netft_ros1_smoke_harness exactly-one
elif [[ "${ROS_VERSION:-}" == "2" ]]; then
  cd "$temp_root/ws"
  colcon build \
    --base-paths src \
    --packages-select netft_driver \
    --event-handlers console_direct+ \
    --cmake-args -DCMAKE_BUILD_TYPE=Release
  assert_native_install \
    "$temp_root/ws/install/netft_driver/share/netft_driver" 2
  colcon test \
    --packages-select netft_driver \
    --event-handlers console_direct+
  colcon test-result --verbose
  ctest --test-dir build/netft_driver -N >build/netft_driver/registered-tests.txt
  grep -q 'netft_unit' build/netft_driver/registered-tests.txt
  grep -q 'netft_ros2_smoke_harness' \
    build/netft_driver/registered-tests.txt
  validate_junit_result netft_unit at-least-one
  validate_junit_result netft_ros2_smoke_harness exactly-one
else
  echo "ROS_VERSION must be 1 or 2" >&2
  exit 2
fi
