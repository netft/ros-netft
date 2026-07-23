#!/usr/bin/env bash
set -euo pipefail

test_root=""
active_pids=()
isolation_shutdown_poll_attempts="${NETFT_ISOLATION_SHUTDOWN_POLL_ATTEMPTS:-400}"
isolation_shutdown_poll_interval="${NETFT_ISOLATION_SHUTDOWN_POLL_INTERVAL:-0.05}"

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

process_group_has_live_members() {
  local group_id="$1"
  local process_group
  local remainder
  local state
  local stat_line
  local stat_path
  for stat_path in /proc/[0-9]*/stat; do
    IFS= read -r stat_line 2>/dev/null <"$stat_path" || continue
    remainder="${stat_line##*) }"
    [[ "$remainder" != "$stat_line" ]] || continue
    read -r state _ process_group _ <<<"$remainder"
    if [[ "$process_group" == "$group_id" && "$state" != Z && "$state" != X ]]; then
      return 0
    fi
  done
  return 1
}

terminate_active_pid() {
  local pid="$1"
  local pgid="$pid"
  local attempt
  local leader_reaped=0
  local state
  kill -TERM -- "-$pgid" 2>/dev/null || true
  for ((attempt = 0; attempt < isolation_shutdown_poll_attempts; attempt++)); do
    if ((leader_reaped == 0)); then
      state="$(process_state "$pid" 2>/dev/null)" || state=""
      if [[ "$state" == "Z" ]] || ! kill -0 "$pid" 2>/dev/null; then
        wait "$pid" 2>/dev/null || true
        leader_reaped=1
      fi
    fi
    if ! process_group_has_live_members "$pgid"; then
      if ((leader_reaped == 0)); then
        wait "$pid" 2>/dev/null || true
      fi
      return 0
    fi
    sleep "$isolation_shutdown_poll_interval"
  done

  kill -KILL -- "-$pgid" 2>/dev/null || true
  for ((attempt = 0; attempt < isolation_shutdown_poll_attempts; attempt++)); do
    if ((leader_reaped == 0)); then
      state="$(process_state "$pid" 2>/dev/null)" || state=""
      if [[ "$state" == "Z" ]] || ! kill -0 "$pid" 2>/dev/null; then
        wait "$pid" 2>/dev/null || true
        leader_reaped=1
      fi
    fi
    if ! process_group_has_live_members "$pgid"; then
      if ((leader_reaped == 0)); then
        wait "$pid" 2>/dev/null || true
      fi
      return 0
    fi
    sleep "$isolation_shutdown_poll_interval"
  done
  echo "control smoke process group $pgid survived the final shutdown deadline" >&2
  return 1
}

cleanup() {
  local status=$?
  local cleanup_failed=0
  local pid
  trap - EXIT INT TERM
  for pid in "${active_pids[@]}"; do
    [[ -n "$pid" ]] || continue
    if ! terminate_active_pid "$pid"; then
      cleanup_failed=1
    fi
  done
  if [[ -n "$test_root" ]] && ! rm -rf -- "$test_root"; then
    cleanup_failed=1
  fi
  if ((status == 0 && cleanup_failed != 0)); then
    status=1
  fi
  exit "$status"
}

start_control_run() {
  local installed_smoke="$1"
  local run_root="$2"
  local output="$3"
  setsid timeout --foreground --signal=TERM --kill-after=20s 120s \
    env NETFT_ROS2_CONTROL_SMOKE_ROOT="$run_root" \
    "$installed_smoke" --installed >"$output" 2>&1 &
  active_pids+=("$!")
}

wait_control_run() {
  local index="$1"
  local pid="${active_pids[$index]}"
  local cleanup_status=0
  local status
  if wait "$pid"; then
    status=0
  else
    status=$?
  fi
  terminate_active_pid "$pid" || cleanup_status=$?
  active_pids[$index]=""
  if ((status == 0 && cleanup_status != 0)); then
    status=1
  fi
  return "$status"
}

domain_from_log() {
  sed -n 's/^netft_control_domain=\([0-9][0-9]*\)$/\1/p' "$1"
}

assert_run_output() {
  local log="$1"
  grep -Fxq 'netft_control_event=publish_ready' "$log"
  grep -Fxq 'netft_control_state=left:unconfigured,right:active' "$log"
  grep -Fxq 'netft_control_event=right_survived' "$log"
  grep -Fxq 'netft_control_result=pass' "$log"
}

assert_domain_has_no_daemon() {
  local domain="$1"
  local pattern="[r]os2cli[.]daemon[.]daemonize.*--ros-domain-id[ =]+${domain}([[:space:]]|$)"
  if pgrep -f "$pattern" >/dev/null; then
    echo "ROS 2 daemon leaked for domain $domain" >&2
    pgrep -af "$pattern" >&2 || true
    return 1
  fi
}

main() {
  local repo_root
  local installed_smoke
  local single_root
  local single_domain
  local run_root
  local side
  local status_a
  local status_b
  local domain_a
  local domain_b
  repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
  test_root="$(mktemp -d)"
  active_pids=()
  trap cleanup EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM

  mkdir -p "$test_root/ws/src"
  ln -s "$repo_root" "$test_root/ws/src/netft_driver"
  (
    cd "$test_root/ws"
    timeout --kill-after=10s 180s colcon build \
      --base-paths src --packages-select netft_driver \
      --cmake-args -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
      -DNETFT_INSTALL_TEST_TOOLS=ON
  )
  set +u
  source "$test_root/ws/install/setup.bash"
  set -u
  installed_smoke="$(ros2 pkg prefix --share netft_driver)/test/integration/ros2_control_smoke.sh"

  single_root="$(mktemp -d)"
  start_control_run "$installed_smoke" "$single_root" "$test_root/single.log"
  if wait_control_run 0; then
    status_a=0
  else
    status_a=$?
    cat "$test_root/single.log" >&2
    return "$status_a"
  fi
  single_domain="$(domain_from_log "$test_root/single.log")"
  [[ -n "$single_domain" ]]
  assert_run_output "$test_root/single.log"
  assert_domain_has_no_daemon "$single_domain"

  active_pids=()
  for side in a b; do
    run_root="$(mktemp -d)"
    start_control_run "$installed_smoke" "$run_root" "$test_root/$side.log"
  done
  if wait_control_run 0; then status_a=0; else status_a=$?; fi
  if wait_control_run 1; then status_b=0; else status_b=$?; fi
  [[ "$status_a" -eq 0 ]] || { cat "$test_root/a.log" >&2; return "$status_a"; }
  [[ "$status_b" -eq 0 ]] || { cat "$test_root/b.log" >&2; return "$status_b"; }
  domain_a="$(domain_from_log "$test_root/a.log")"
  domain_b="$(domain_from_log "$test_root/b.log")"
  [[ -n "$domain_a" && -n "$domain_b" && "$domain_a" != "$domain_b" ]] || {
    echo "concurrent control smokes did not report distinct leased domains" >&2
    cat "$test_root/a.log" "$test_root/b.log" >&2
    return 1
  }
  assert_run_output "$test_root/a.log"
  assert_run_output "$test_root/b.log"
  assert_domain_has_no_daemon "$domain_a"
  assert_domain_has_no_daemon "$domain_b"
  echo "netft_control_isolation=pass single=$single_domain concurrent=$domain_a,$domain_b"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  main "$@"
fi
