#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
smoke_script="$repo_root/test/integration/ros1_smoke.sh"

fail() {
  echo "$*" >&2
  exit 1
}

script_source="$(<"$smoke_script")"
[[ "$script_source" == *"shutdown_child()"* ]] ||
  fail "smoke harness has no bounded shutdown helper"
[[ "$script_source" == *"timeout --kill-after="*" rosservice call "* ]] ||
  fail "rosservice call has no hard deadline and kill-after"
if grep -Eq '^[[:space:]]*wait[[:space:]]+"\$(node_pid|sensor_pid|roscore_pid)"' \
  "$smoke_script"; then
  fail "smoke harness still waits directly for a managed child"
fi
if grep -Eq '^[[:space:]]*timeout[[:space:]]+10[[:space:]]+rostopic' \
  "$smoke_script"; then
  fail "rostopic call has a deadline without a kill-after"
fi
readiness_probe_count=0
while IFS= read -r source_line; do
  case "$source_line" in
    *"rosparam list"*|*"rosservice info"*)
      readiness_probe_count=$((readiness_probe_count + 1))
      [[ "$source_line" == *"timeout --kill-after="* ]] ||
        fail "readiness probe has no hard deadline: $source_line"
      ;;
  esac
done <"$smoke_script"
[[ "$readiness_probe_count" -eq 4 ]] ||
  fail "expected 4 readiness probes, found $readiness_probe_count"

source "$smoke_script"

test_root="$(mktemp -d)"
helper_pid_file="$test_root/helper.pid"
helper_result_file="$test_root/helper.result"
exit_status_marker="$test_root/exit-status.reaped"
survivor_wait_marker="$test_root/survivor.waited"
cleanup_pid_file="$test_root/cleanup.pids"
cleanup_temp_root="$test_root/cleanup"

emergency_cleanup() {
  local pid
  for pid_file in "$helper_pid_file" "$cleanup_pid_file"; do
    [[ -f "$pid_file" ]] || continue
    while read -r pid; do
      [[ -n "$pid" ]] || continue
      kill -KILL "$pid" 2>/dev/null || true
    done <"$pid_file"
  done
  rm -rf -- "$test_root"
}
trap emergency_cleanup EXIT

set +e
timeout --kill-after=0.2s 3s bash -s -- \
  "$smoke_script" "$exit_status_marker" <<'BASH'
set -euo pipefail
source "$1"
reaped_marker="$2"
shutdown_poll_attempts=1
shutdown_poll_interval=0
kill() {
  if [[ "${1:-}" == "-0" ]] && [[ -e "$reaped_marker" ]]; then
    return 1
  fi
  return 0
}
process_state() {
  printf 'Z\n'
}
wait() {
  : >"$reaped_marker"
  return 17
}
set +e
shutdown_child 454545 INT
child_status=$?
set -e
[[ "$child_status" -eq 17 ]] || {
  echo "shutdown helper returned $child_status, expected reaped child status 17" >&2
  exit 1
}
BASH
exit_status_test=$?
set -e
[[ "$exit_status_test" -eq 0 ]] ||
  fail "shutdown helper did not preserve a zombie child's exit status 17"

set +e
timeout --kill-after=0.2s 3s bash -s -- \
  "$smoke_script" "$survivor_wait_marker" <<'BASH'
set -euo pipefail
source "$1"
wait_marker="$2"
shutdown_poll_attempts=1
shutdown_poll_interval=0
kill() {
  return 0
}
process_state() {
  printf 'D\n'
}
wait() {
  : >"$wait_marker"
  return 17
}
set +e
shutdown_child 464646 INT
survivor_status=$?
set -e
[[ "$survivor_status" -eq 126 ]] || {
  echo "shutdown helper returned $survivor_status, expected survivor status 126" >&2
  exit 1
}
[[ ! -e "$wait_marker" ]] || {
  echo "shutdown helper attempted to reap a process it still observed alive" >&2
  exit 1
}
BASH
survivor_test=$?
set -e
[[ "$survivor_test" -eq 0 ]] ||
  fail "shutdown helper did not distinguish a survivor from child exit status"

cleanup_failure_root="$test_root/cleanup-failure"
for cleanup_case in "0 1" "23 23"; do
  read -r incoming_status expected_status <<<"$cleanup_case"
  case_root="$cleanup_failure_root-$incoming_status"
  mkdir -p "$case_root"
  set +e
  timeout --kill-after=0.2s 3s bash -s -- \
    "$smoke_script" "$case_root" "$incoming_status" <<'BASH'
set -euo pipefail
source "$1"
temp_root="$2"
incoming_status="$3"
node_pid=474747
sensor_pid=""
roscore_pid=""
shutdown_child() {
  [[ -n "${1:-}" ]] || return 0
  return 125
}
trap cleanup EXIT
exit "$incoming_status"
BASH
  cleanup_failure_status=$?
  set -e
  [[ "$cleanup_failure_status" -eq "$expected_status" ]] ||
    fail "cleanup failure changed $incoming_status to $cleanup_failure_status; expected $expected_status"
  [[ ! -e "$case_root" ]] || fail "cleanup failure fixture did not remove temp root"
done

timeout --kill-after=0.2s 3s bash -s -- "$smoke_script" <<'BASH'
set -euo pipefail
source "$1"
sensor_pid=484848
shutdown_child() {
  return 126
}
if shutdown_sensor; then
  echo "sensor shutdown accepted survivor status" >&2
  exit 1
else
  sensor_status=$?
fi
[[ "$sensor_status" -eq 126 ]] || {
  echo "sensor shutdown returned $sensor_status, expected 126" >&2
  exit 1
}
[[ "$sensor_pid" == "484848" ]] || {
  echo "sensor shutdown discarded survivor PID" >&2
  exit 1
}
for reaped_status in 17 124 125; do
  sensor_pid="$((484848 + reaped_status))"
  shutdown_child() {
    return "$reaped_status"
  }
  if shutdown_sensor; then
    echo "sensor shutdown accepted reaped status $reaped_status" >&2
    exit 1
  else
    sensor_status=$?
  fi
  [[ "$sensor_status" -eq "$reaped_status" ]] || {
    echo "sensor shutdown returned $sensor_status, expected $reaped_status" >&2
    exit 1
  }
  [[ -z "$sensor_pid" ]] || {
    echo "sensor shutdown retained PID for reaped status $reaped_status" >&2
    exit 1
  }
done
sensor_pid=484848
shutdown_child() {
  return 0
}
shutdown_sensor
[[ -z "$sensor_pid" ]] || {
  echo "sensor shutdown retained a reaped PID" >&2
  exit 1
}
BASH

timeout --kill-after=0.2s 3s bash -s -- "$smoke_script" <<'BASH'
set -euo pipefail
source "$1"
node_pid=494949
shutdown_child() {
  return 126
}
if shutdown_node; then
  echo "node shutdown accepted survivor status" >&2
  exit 1
else
  node_status=$?
fi
[[ "$node_status" -eq 126 ]] || {
  echo "node shutdown returned $node_status, expected 126" >&2
  exit 1
}
[[ "$node_pid" == "494949" ]] || {
  echo "node shutdown discarded survivor PID" >&2
  exit 1
}
shutdown_child() {
  return 17
}
if shutdown_node; then
  echo "node shutdown accepted child exit status 17" >&2
  exit 1
else
  node_status=$?
fi
[[ "$node_status" -eq 17 ]] || {
  echo "node shutdown returned $node_status, expected 17" >&2
  exit 1
}
[[ -z "$node_pid" ]] || {
  echo "node shutdown retained a reaped PID" >&2
  exit 1
}
BASH

timeout --kill-after=2s 8s bash -s -- \
  "$smoke_script" "$helper_pid_file" <<'BASH'
set -euo pipefail
source "$1"
bash -c 'trap "" INT; trap "exit 0" TERM; while :; do sleep 1; done' &
term_child_pid=$!
printf '%s\n' "$term_child_pid" >"$2"
set +e
shutdown_child "$term_child_pid" INT
term_status=$?
set -e
if [[ "$term_status" -ne 124 ]]; then
  echo "shutdown helper returned $term_status, expected TERM-escalation status 124" >&2
  exit 1
fi
if kill -0 "$term_child_pid" 2>/dev/null; then
  echo "TERM-escalation fixture remained alive" >&2
  exit 1
fi
BASH

timeout --kill-after=2s 8s bash -s -- \
  "$smoke_script" "$helper_pid_file" "$helper_result_file" <<'BASH'
set -euo pipefail
source "$1"
bash -c 'trap "" INT TERM; while :; do sleep 1; done' &
stubborn_pid=$!
printf '%s\n' "$stubborn_pid" >"$2"
started_at="$(date +%s)"
set +e
shutdown_child "$stubborn_pid" INT
stubborn_status=$?
set -e
elapsed="$(( $(date +%s) - started_at ))"
if [[ "$stubborn_status" -ne 125 ]]; then
  echo "shutdown helper returned $stubborn_status, expected KILL-escalation status 125" >&2
  exit 1
fi
if kill -0 "$stubborn_pid" 2>/dev/null; then
  echo "shutdown helper left its child running" >&2
  exit 1
fi
for running_pid in $(jobs -pr); do
  if [[ "$running_pid" == "$stubborn_pid" ]]; then
    echo "shutdown helper did not reap its child" >&2
    exit 1
  fi
done
printf '%s\n' "$elapsed" >"$3"
BASH

helper_elapsed="$(<"$helper_result_file")"
(( helper_elapsed < 8 )) || fail "shutdown helper exceeded its hard bound"

mkdir -p "$cleanup_temp_root"
touch "$cleanup_temp_root/sentinel"
set +e
timeout --kill-after=2s 8s bash -s -- \
  "$smoke_script" "$cleanup_temp_root" "$cleanup_pid_file" <<'BASH'
set -euo pipefail
source "$1"
temp_root="$2"
node_pid=""
sensor_pid=""
roscore_pid=""
trap cleanup EXIT
bash -c 'trap "" INT TERM; while :; do sleep 1; done' &
node_pid=$!
bash -c 'trap "exit 0" INT TERM; while :; do sleep 1; done' &
sensor_pid=$!
printf '%s\n%s\n' "$node_pid" "$sensor_pid" >"$3"
exit 23
BASH
cleanup_status=$?
set -e
[[ "$cleanup_status" -eq 23 ]] ||
  fail "cleanup changed exit status 23 to $cleanup_status"
[[ ! -e "$cleanup_temp_root" ]] || fail "cleanup did not remove its temp tree"
while read -r child_pid; do
  if kill -0 "$child_pid" 2>/dev/null; then
    fail "cleanup left child $child_pid running"
  fi
done <"$cleanup_pid_file"

valid_history="$test_root/valid.json"
printf '%s\n' '[2, 66, 2, 0]' >"$valid_history"
validate_command_history "$valid_history"
history_case=0
for invalid_commands in \
  '[2, 2, 66, 2, 0]' \
  '[2, 66, 66, 2, 0]' \
  '[2, 66, 2, 0, 2]' \
  '[2, 66, 2, 0, 0]' \
  '[66, 2, 0, 2]'; do
  history_case=$((history_case + 1))
  invalid_history="$test_root/invalid-$history_case.json"
  printf '%s\n' "$invalid_commands" >"$invalid_history"
  if validate_command_history "$invalid_history" >/dev/null 2>&1; then
    fail "command-history validation accepted extra or misordered commands: $invalid_commands"
  fi
done

echo "ROS 1 smoke harness regression checks passed"
