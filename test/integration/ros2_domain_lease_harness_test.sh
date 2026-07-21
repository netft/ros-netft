#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
self_script="$repo_root/test/integration/ros2_domain_lease_harness_test.sh"
lease_helper="$repo_root/test/integration/ros2_domain_lease.sh"

if [[ "${1:-}" == "__hold" ]]; then
  result_file="$2"
  ready_file="$3"
  release_file="$4"
  source "$lease_helper"
  acquire_ros2_domain leased_domain
  printf '%s\n' "$leased_domain" >"$result_file"
  : >"$ready_file"
  while [[ ! -e "$release_file" ]]; do
    sleep 0.02
  done
  release_ros2_domain_leases
  exit 0
fi

test_root="$(mktemp -d)"
holder_pids=()
cleanup() {
  local pid
  for pid in "${holder_pids[@]}"; do
    kill -TERM "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  done
  rm -rf -- "$test_root"
}
trap cleanup EXIT INT TERM

export NETFT_ROS2_DOMAIN_LEASE_DIR="$test_root/leases"
export NETFT_ROS2_DOMAIN_POOL_MIN=220
export NETFT_ROS2_DOMAIN_POOL_MAX=221

start_holder() {
  local name="$1"
  bash "$self_script" __hold \
    "$test_root/$name.domain" "$test_root/$name.ready" "$test_root/$name.release" &
  holder_pids+=("$!")
}

start_holder first
start_holder second
deadline=$((SECONDS + 5))
while [[ ! -e "$test_root/first.ready" || ! -e "$test_root/second.ready" ]]; do
  if ((SECONDS >= deadline)); then
    echo "concurrent domain holders did not become ready" >&2
    exit 1
  fi
  sleep 0.02
done

first_domain="$(<"$test_root/first.domain")"
second_domain="$(<"$test_root/second.domain")"
[[ "$first_domain" != "$second_domain" ]] || {
  echo "concurrent leases shared domain $first_domain" >&2
  exit 1
}

source "$lease_helper"
set +e
exhaustion_output="$(acquire_ros2_domain exhausted_domain 2>&1)"
exhaustion_status=$?
set -e
[[ "$exhaustion_status" -ne 0 ]] || {
  echo "exhausted domain pool unexpectedly granted a lease" >&2
  exit 1
}
grep -q "ROS 2 domain lease pool exhausted" <<<"$exhaustion_output"

: >"$test_root/first.release"
wait "${holder_pids[0]}"
holder_pids[0]=""
acquire_ros2_domain reused_domain
[[ "$reused_domain" == "$first_domain" ]] || {
  echo "released domain $first_domain was not reused; got $reused_domain" >&2
  exit 1
}
release_ros2_domain_leases

: >"$test_root/second.release"
wait "${holder_pids[1]}"
holder_pids[1]=""
echo "ROS 2 domain lease concurrency checks passed"
