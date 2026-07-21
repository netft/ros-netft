#!/usr/bin/env bash

if [[ -n "${NETFT_ROS2_DOMAIN_LEASE_LOADED:-}" ]]; then
  return 0
fi
NETFT_ROS2_DOMAIN_LEASE_LOADED=1

NETFT_ROS2_DOMAIN_POOL_MIN="${NETFT_ROS2_DOMAIN_POOL_MIN:-100}"
NETFT_ROS2_DOMAIN_POOL_MAX="${NETFT_ROS2_DOMAIN_POOL_MAX:-199}"
NETFT_ROS2_DOMAIN_LEASE_DIR="${NETFT_ROS2_DOMAIN_LEASE_DIR:-${TMPDIR:-/tmp}/netft-ros2-domain-leases}"
NETFT_ROS2_DOMAIN_LEASE_FDS=()
NETFT_ROS2_LEASED_DOMAINS=()

validate_ros2_domain_pool() {
  [[ "$NETFT_ROS2_DOMAIN_POOL_MIN" =~ ^[0-9]+$ ]] &&
    [[ "$NETFT_ROS2_DOMAIN_POOL_MAX" =~ ^[0-9]+$ ]] &&
    ((NETFT_ROS2_DOMAIN_POOL_MIN <= NETFT_ROS2_DOMAIN_POOL_MAX)) &&
    ((NETFT_ROS2_DOMAIN_POOL_MAX <= 232))
}

acquire_ros2_domain() {
  local result_variable="${1:-}"
  local candidate
  local lease_fd
  [[ -n "$result_variable" ]] || {
    echo "acquire_ros2_domain requires a result variable" >&2
    return 2
  }
  command -v flock >/dev/null 2>&1 || {
    echo "flock from util-linux is required for ROS 2 smoke domain leases" >&2
    return 2
  }
  validate_ros2_domain_pool || {
    echo "invalid ROS 2 domain lease pool: $NETFT_ROS2_DOMAIN_POOL_MIN-$NETFT_ROS2_DOMAIN_POOL_MAX" >&2
    return 2
  }
  mkdir -p "$NETFT_ROS2_DOMAIN_LEASE_DIR"
  for ((candidate = NETFT_ROS2_DOMAIN_POOL_MIN;
       candidate <= NETFT_ROS2_DOMAIN_POOL_MAX; candidate++)); do
    lease_fd=""
    exec {lease_fd}>"$NETFT_ROS2_DOMAIN_LEASE_DIR/domain-$candidate.lock"
    if flock --nonblock "$lease_fd"; then
      NETFT_ROS2_DOMAIN_LEASE_FDS+=("$lease_fd")
      NETFT_ROS2_LEASED_DOMAINS+=("$candidate")
      printf -v "$result_variable" '%s' "$candidate"
      return 0
    fi
    exec {lease_fd}>&-
  done
  echo "ROS 2 domain lease pool exhausted: $NETFT_ROS2_DOMAIN_POOL_MIN-$NETFT_ROS2_DOMAIN_POOL_MAX" >&2
  return 1
}

stop_ros2_daemon_for_domain() {
  local domain="$1"
  local attempt
  local consecutive_stopped=0
  if ! ROS_DOMAIN_ID="$domain" timeout --kill-after=2s 5s ros2 daemon stop \
      >/dev/null 2>&1; then
    echo "failed to stop ROS 2 daemon for domain $domain" >&2
    return 1
  fi
  for ((attempt = 0; attempt < 20; attempt++)); do
    if ROS_DOMAIN_ID="$domain" timeout --kill-after=1s 2s ros2 daemon status \
        2>/dev/null | grep -q "is not running"; then
      consecutive_stopped=$((consecutive_stopped + 1))
      if ((consecutive_stopped == 2)); then
        return 0
      fi
    else
      consecutive_stopped=0
    fi
    sleep 0.05
  done
  echo "ROS 2 daemon did not stop for domain $domain" >&2
  return 1
}

release_ros2_domain_leases() {
  local lease_fd
  local failed=0
  for lease_fd in "${NETFT_ROS2_DOMAIN_LEASE_FDS[@]}"; do
    if ! flock --unlock "$lease_fd"; then
      failed=1
    fi
    exec {lease_fd}>&-
  done
  NETFT_ROS2_DOMAIN_LEASE_FDS=()
  NETFT_ROS2_LEASED_DOMAINS=()
  return "$failed"
}
