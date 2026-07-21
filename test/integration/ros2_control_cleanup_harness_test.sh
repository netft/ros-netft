#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
self_script="$repo_root/test/integration/ros2_control_cleanup_harness_test.sh"
control_script="$repo_root/test/integration/ros2_control_smoke.sh"
lease_helper="$repo_root/test/integration/ros2_domain_lease.sh"
isolation_script="$repo_root/test/integration/ros2_control_isolation_test.sh"

if [[ "${1:-}" == "__case" ]]; then
  mode="$2"
  requested_smoke_root="$3"
  cleanup_marker="$4"
  ready_file="$5"
  source "$control_script"
  smoke_root="$requested_smoke_root"
  managed_pids=()
  NETFT_ROS2_DOMAIN_LEASE_FDS=()
  NETFT_ROS2_LEASED_DOMAINS=(220)
  mkdir -p "$smoke_root"
  printf '%s\n' RSP_LOG_SENTINEL >"$smoke_root/robot_state_publisher.log"
  printf '%s\n' MANAGER_LOG_SENTINEL >"$smoke_root/controller_manager.log"

  stop_ros2_daemon_for_domain() {
    printf 'stop:%s\n' "$1" >>"$cleanup_marker"
    [[ "$mode" != cleanup-failure-* ]]
  }
  release_ros2_domain_leases() {
    printf 'release\n' >>"$cleanup_marker"
    return 0
  }

  trap cleanup EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM
  case "$mode" in
    cleanup-failure-success) exit 0 ;;
    cleanup-failure-original) exit 17 ;;
    signal-int | signal-term)
      : >"$ready_file"
      while :; do sleep 0.05; done
      ;;
    *) echo "unknown cleanup harness mode: $mode" >&2; exit 2 ;;
  esac
fi

test_root="$(mktemp -d)"
active_pid=""
terminate_active_pid() {
  local pid="$1"
  local pgid
  pgid="$(ps -o pgid= -p "$pid" 2>/dev/null | tr -d ' ')"
  if [[ "$pgid" == "$pid" ]]; then
    kill -TERM -- "-$pid" 2>/dev/null || true
  else
    kill -TERM "$pid" 2>/dev/null || true
  fi
  local deadline=$((SECONDS + 2))
  while kill -0 "$pid" 2>/dev/null && ((SECONDS < deadline)); do
    [[ "$(ps -o stat= -p "$pid" 2>/dev/null)" == Z* ]] && break
    sleep 0.02
  done
  if kill -0 "$pid" 2>/dev/null; then
    if [[ "$pgid" == "$pid" ]]; then
      kill -KILL -- "-$pid" 2>/dev/null || true
    else
      kill -KILL "$pid" 2>/dev/null || true
    fi
  fi
  wait "$pid" 2>/dev/null || true
}
cleanup_test_root() {
  if [[ -n "$active_pid" ]]; then
    terminate_active_pid "$active_pid"
  fi
  rm -rf -- "$test_root"
}
trap cleanup_test_root EXIT INT TERM

run_exit_case() {
  local mode="$1"
  local expected_status="$2"
  local case_root="$test_root/$mode-root"
  local marker="$test_root/$mode.marker"
  local output="$test_root/$mode.log"
  local status
  set +e
  bash "$self_script" __case "$mode" "$case_root" "$marker" \
    "$test_root/$mode.ready" >"$output" 2>&1
  status=$?
  set -e
  [[ "$status" -eq "$expected_status" ]] || {
    cat "$output" >&2
    echo "$mode exited $status, expected $expected_status" >&2
    exit 1
  }
  grep -Fxq 'stop:220' "$marker"
  grep -Fxq 'release' "$marker"
  grep -q RSP_LOG_SENTINEL "$output"
  grep -q MANAGER_LOG_SENTINEL "$output"
  [[ ! -e "$case_root" ]]
}

run_signal_case() {
  local mode="$1"
  local signal="$2"
  local expected_status="$3"
  local case_root="$test_root/$mode-root"
  local marker="$test_root/$mode.marker"
  local ready="$test_root/$mode.ready"
  local output="$test_root/$mode.log"
  local deadline
  local status
  set -m
  bash "$self_script" __case "$mode" "$case_root" "$marker" "$ready" \
    >"$output" 2>&1 &
  active_pid="$!"
  set +m
  deadline=$((SECONDS + 5))
  while [[ ! -e "$ready" ]]; do
    if ! kill -0 "$active_pid" 2>/dev/null || ((SECONDS >= deadline)); then
      cat "$output" >&2
      echo "$mode did not become ready" >&2
      exit 1
    fi
    sleep 0.02
  done
  kill "-$signal" "$active_pid"
  deadline=$((SECONDS + 5))
  while kill -0 "$active_pid" 2>/dev/null; do
    [[ "$(ps -o stat= -p "$active_pid" 2>/dev/null)" == Z* ]] && break
    if ((SECONDS >= deadline)); then
      cat "$output" >&2
      echo "$mode did not exit after SIG$signal" >&2
      terminate_active_pid "$active_pid"
      active_pid=""
      exit 1
    fi
    sleep 0.02
  done
  set +e
  wait "$active_pid"
  status=$?
  set -e
  active_pid=""
  [[ "$status" -eq "$expected_status" ]] || {
    cat "$output" >&2
    echo "$mode exited $status, expected $expected_status" >&2
    exit 1
  }
  grep -Fxq 'stop:220' "$marker"
  grep -Fxq 'release' "$marker"
  grep -q RSP_LOG_SENTINEL "$output"
  grep -q MANAGER_LOG_SENTINEL "$output"
  [[ ! -e "$case_root" ]]
}

run_exit_case cleanup-failure-success 1
run_exit_case cleanup-failure-original 17
run_signal_case signal-int INT 130
run_signal_case signal-term TERM 143

set +e
stop_output="$(bash -c '
  source "$1"
  timeout() { return 1; }
  stop_ros2_daemon_for_domain 220
' _ "$lease_helper" 2>&1)"
stop_status=$?
set -e
[[ "$stop_status" -eq 1 ]]
grep -Fxq 'failed to stop ROS 2 daemon for domain 220' <<<"$stop_output"

zombie_output="$(bash -c '
  source "$1"
  isolation_shutdown_poll_attempts=5
  isolation_shutdown_poll_interval=0.02
  child_file="$2"
  parent_pid=""
  cleanup_fixture() {
    if [[ -n "$parent_pid" ]]; then
      kill -KILL "$parent_pid" 2>/dev/null || true
      wait "$parent_pid" 2>/dev/null || true
    fi
  }
  trap cleanup_fixture EXIT
  python3 -c "
import os
import pathlib
import sys
import time
child = os.fork()
if child == 0:
    os.setsid()
    os._exit(0)
pathlib.Path(sys.argv[1]).write_text(str(child), encoding=\"utf-8\")
time.sleep(300)
" "$child_file" &
  parent_pid="$!"
  deadline=$((SECONDS + 5))
  while [[ ! -s "$child_file" ]]; do
    ((SECONDS < deadline)) || exit 1
    sleep 0.02
  done
  child_pid="$(<"$child_file")"
  while [[ "$(process_state "$child_pid" 2>/dev/null)" != Z ]]; do
    ((SECONDS < deadline)) || exit 1
    sleep 0.02
  done
  terminate_active_pid "$child_pid"
  echo "zombie-only process group treated as stopped"
' _ "$isolation_script" "$test_root/zombie.child")"
grep -Fxq 'zombie-only process group treated as stopped' <<<"$zombie_output"

isolation_output="$(bash -c '
  source "$1"
  isolation_shutdown_poll_attempts=5
  isolation_shutdown_poll_interval=0.02
  ready_file="$2"
  child_file="$3"
  setsid bash -c '\''
    python3 -c "
import pathlib
import signal
import sys
import time
signal.signal(signal.SIGHUP, signal.SIG_IGN)
signal.signal(signal.SIGTERM, signal.SIG_IGN)
pathlib.Path(sys.argv[1]).touch()
time.sleep(300)
" "$1" &
    echo "$!" >"$2"
  '\'' _ "$ready_file" "$child_file" &
  leader_pid="$!"
  deadline=$((SECONDS + 5))
  while [[ ! -e "$ready_file" || ! -s "$child_file" ]]; do
    if ((SECONDS >= deadline)); then
      exit 1
    fi
    sleep 0.02
  done
  wait "$leader_pid"
  child_pid="$(<"$child_file")"
  kill -0 "$child_pid"
  active_pids=("$leader_pid")
  terminate_active_pid "$leader_pid"
  active_pids[0]=""
  ! kill -0 "$child_pid" 2>/dev/null
  ! kill -0 -- "-$leader_pid" 2>/dev/null
  echo "bounded isolation cleanup passed"
' _ "$isolation_script" "$test_root/isolation.ready" "$test_root/isolation.child")"
grep -Fxq 'bounded isolation cleanup passed' <<<"$isolation_output"
echo "ROS 2 control cleanup behavior checks passed"
