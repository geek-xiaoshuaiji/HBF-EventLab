#!/usr/bin/env bash
# Same unified backend and GPU inputs as upstream run_unified_replay.sh;
# stream only the lifecycle CSV through one-core gzip to limit /home usage.
set -euo pipefail
if (( $# < 5 || $# > 6 )); then
  echo "usage: $0 accel-sim.out gpu.config kernelslist.g image-pages.txt NEW_RUN_DIR [backend.ini]" >&2
  exit 2
fi
BIN=$(realpath "$1")
GPU_CONFIG=$(realpath "$2")
TRACES=$(realpath "$3")
IMAGE=$(realpath "$4")
HERE=$(cd "$(dirname "$0")" && pwd)
INI=$(realpath "${6:-$HERE/../config/gpt2-eventlab-device.ini}")
test ! -e "$5"
mkdir -p "$5"
RUN=$(realpath "$5")
cp "$INI" "$RUN/backend-input.ini"
cp "$GPU_CONFIG" "$RUN/gpgpusim.config"
mkfifo "$RUN/eventlab.fifo"
gzip -1 < "$RUN/eventlab.fifo" > "$RUN/eventlab.csv.gz" &
ZIP_PID=$!
export HBF_BACKEND=eventlab HBF_EVENTLAB_DEVICE_CONFIG="$RUN/backend-input.ini"
export HBF_EVENTLAB_IMAGE_PAGE_FILE="$IMAGE" HBF_EVENTLAB_PRELOAD_IMAGE=1 HBF_EVENTLAB_SEED_IMAGE=0
export HBF_EVENTLAB_GPU_MSHRS="${HBF_EVENTLAB_GPU_MSHRS:-512}" HBF_EVENTLAB_TRACE="$RUN/eventlab.fifo"
unset HBF_EVENTLAB_PAGES_PER_CHANNEL HBF_EVENTLAB_UCIE_GRADE HBF_EVENTLAB_BURST_WRITES
unset HBF_EVENTLAB_RESIDENT_FRAGMENTS HBF_EVENTLAB_FAIL_ONCE
set +e
timeout "${RUN_TIMEOUT:-120}" "$BIN" -config "$GPU_CONFIG" -trace "$TRACES" \
  -gpgpu_hbf_channel_bw_gbps "${GPU_CHANNEL_BW_GBPS:-188}" \
  -gpgpu_hbf_trace_file "$RUN/hbf.csv" -gpgpu_deadlock_detect 1 >"$RUN/run.log" 2>&1
rc=$?
if (( rc == 0 )); then
  wait "$ZIP_PID"
  zip_rc=$?
  if (( zip_rc != 0 )); then rc=$zip_rc; fi
  if (( rc == 0 )); then gzip -t "$RUN/eventlab.csv.gz"; rc=$?; fi
else
  kill "$ZIP_PID" 2>/dev/null
  wait "$ZIP_PID" 2>/dev/null
fi
set -e
printf '%s\n' "$rc" > "$RUN/run.rc"
exit "$rc"
