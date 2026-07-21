#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
smoke_script="$repo_root/test/integration/ros2_smoke.sh"
self_script="$repo_root/test/integration/ros2_smoke_harness_test.sh"

fail() {
  echo "$*" >&2
  exit 1
}

live_helper_inner() {
  local helper_kind="$1"
  local wait_marker="$2"
  source "$smoke_script"
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
    while :; do
      sleep 1
    done
  }

  set +e
  if [[ "$helper_kind" == "child" ]]; then
    shutdown_child 424242 INT
  else
    shutdown_process_group 424242 INT
  fi
  local helper_status=$?
  set -e
  [[ "$helper_status" -ne 0 ]] ||
    fail "$helper_kind helper accepted a permanently live process"
  [[ ! -e "$wait_marker" ]] ||
    fail "$helper_kind helper entered wait for a live process"
}

zombie_helper_inner() {
  local helper_kind="$1"
  local reaped_marker="$2"
  source "$smoke_script"
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
    printf '%s\n' "$1" >>"$reaped_marker"
    return 0
  }

  if [[ "$helper_kind" == "child" ]]; then
    shutdown_child 434343 INT
  else
    shutdown_process_group 434343 INT
  fi
  [[ "$(wc -l <"$reaped_marker")" -eq 1 ]] ||
    fail "$helper_kind zombie was not reaped exactly once"
}

cleanup_status_inner() {
  local cleanup_root="$1"
  local incoming_status="$2"
  source "$smoke_script"

  shutdown_process_group() {
    return 1
  }
  shutdown_child() {
    return 1
  }
  temp_root="$cleanup_root"
  node_pid=111111
  sensor_pid=222222
  trap cleanup EXIT
  exit "$incoming_status"
}

exit_status_helper_inner() {
  local helper_kind="$1"
  local expected_status="$2"
  local reaped_marker="$3"
  source "$smoke_script"
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
    return "$expected_status"
  }

  set +e
  if [[ "$helper_kind" == "child" ]]; then
    shutdown_child 454545 INT
  else
    shutdown_process_group 454545 INT
  fi
  local helper_status=$?
  set -e
  [[ "$helper_status" -eq "$expected_status" ]] ||
    fail "$helper_kind helper returned $helper_status, expected reaped status $expected_status"
}

case "${1:-}" in
  __live_helper)
    live_helper_inner "$2" "$3"
    exit 0
    ;;
  __zombie_helper)
    zombie_helper_inner "$2" "$3"
    exit 0
    ;;
  __cleanup_status)
    cleanup_status_inner "$2" "$3"
    ;;
  __exit_status)
    exit_status_helper_inner "$2" "$3" "$4"
    exit 0
    ;;
esac

test_root="$(mktemp -d)"

emergency_cleanup() {
  rm -rf -- "$test_root"
}
trap emergency_cleanup EXIT

test_live_helper_is_bounded() {
  local helper_kind="$1"
  local wait_marker="$test_root/wait-$helper_kind"
  set +e
  timeout --kill-after=0.2s 3s bash "$self_script" \
    __live_helper "$helper_kind" "$wait_marker"
  local helper_status=$?
  set -e
  [[ "$helper_status" -eq 0 ]] ||
    fail "$helper_kind live-process path exceeded its bound or failed: $helper_status"
  [[ ! -e "$wait_marker" ]] ||
    fail "$helper_kind helper entered wait after its final deadline"
}

test_zombie_helper_is_reaped() {
  local helper_kind="$1"
  local reaped_marker="$test_root/reaped-$helper_kind"
  timeout --kill-after=0.2s 3s bash "$self_script" \
    __zombie_helper "$helper_kind" "$reaped_marker"
}

test_cleanup_status_precedence() {
  local incoming_status="$1"
  local expected_status="$2"
  local cleanup_root="$test_root/cleanup-$incoming_status"
  mkdir -p "$cleanup_root"
  touch "$cleanup_root/sentinel"
  set +e
  timeout --kill-after=0.2s 3s bash "$self_script" \
    __cleanup_status "$cleanup_root" "$incoming_status"
  local cleanup_status=$?
  set -e
  [[ "$cleanup_status" -eq "$expected_status" ]] ||
    fail "cleanup changed incoming $incoming_status to $cleanup_status; expected $expected_status"
  [[ ! -e "$cleanup_root" ]] || fail "cleanup did not remove its temp tree"
}

test_exit_status_is_preserved() {
  local helper_kind="$1"
  local expected_status=17
  local reaped_marker="$test_root/exit-status-$helper_kind"
  timeout --kill-after=0.2s 3s bash "$self_script" \
    __exit_status "$helper_kind" "$expected_status" "$reaped_marker"
}

test_command_history_is_exact() {
  source "$smoke_script"
  local full_history="$test_root/full-history.json"
  local shutdown_history="$test_root/shutdown-history.json"
  printf '%s\n' '[2, 66, 2, 0]' >"$full_history"
  printf '%s\n' '[2, 0]' >"$shutdown_history"
  validate_command_history "$full_history" '[2, 66, 2, 0]'
  validate_command_history "$shutdown_history" '[2, 0]'

  local invalid_history="$test_root/invalid-shutdown-history.json"
  printf '%s\n' '[2, 2, 0]' >"$invalid_history"
  if validate_command_history "$invalid_history" '[2, 0]' >/dev/null 2>&1; then
    fail "shutdown history accepted an extra start command"
  fi
}

run_requested_test() {
  case "${1:-all}" in
    live_child)
      test_live_helper_is_bounded child
      ;;
    live_group)
      test_live_helper_is_bounded group
      ;;
    zombies)
      test_zombie_helper_is_reaped child
      test_zombie_helper_is_reaped group
      ;;
    cleanup_status)
      test_cleanup_status_precedence 0 1
      test_cleanup_status_precedence 23 23
      ;;
    exit_status)
      test_exit_status_is_preserved child
      test_exit_status_is_preserved group
      ;;
    command_history)
      test_command_history_is_exact
      ;;
    all)
      test_live_helper_is_bounded child
      test_live_helper_is_bounded group
      test_zombie_helper_is_reaped child
      test_zombie_helper_is_reaped group
      test_cleanup_status_precedence 0 1
      test_cleanup_status_precedence 23 23
      test_exit_status_is_preserved child
      test_exit_status_is_preserved group
      test_command_history_is_exact
      ;;
    *)
      fail "unknown harness test: $1"
      ;;
  esac
}

run_requested_test "${1:-all}"
echo "ROS 2 smoke harness regression checks passed (${1:-all})"
