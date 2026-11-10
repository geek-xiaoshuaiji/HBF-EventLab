#!/usr/bin/env bash
# Protect the shared /home filesystem if another writer consumes remaining space.
# Sends SIGTERM only to this experiment's exact simulator binary.
set -euo pipefail
BASE=/home/server/hbf-experiments/20261109-gpt2-unified
BIN="$BASE/simulator/gpu-simulator/bin/release/accel-sim.out"
LIMIT=1073741824
cd "$BASE"
while :; do
  if test -f full-compressed-launcher.rc; then exit 0; fi
  free_bytes=$(df -B1 --output=avail /home | tail -n 1)
  if (( free_bytes < LIMIT )); then
    printf 'disk_guard_stop free_bytes=%s\n' "$free_bytes" >> disk-guard.log
    while read -r pid; do
      test -n "$pid" || continue
      actual=$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)
      if [[ "$actual" == "$BIN" ]]; then
        printf 'sending_sigterm_exact_binary pid=%s\n' "$pid" >> disk-guard.log
        kill -TERM "$pid"
      fi
    done < <(pgrep -x accel-sim.out || true)
    exit 0
  fi
  sleep 30
done
