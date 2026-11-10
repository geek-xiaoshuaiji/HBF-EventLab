#!/usr/bin/env bash
set -euo pipefail
BASE=/home/server/hbf-experiments/20261109-gpt2-unified
OLD=/home/server/hbf-experiments/20261028-gpt2-incremental
PREV=/home/server/hbf-experiments/20261107-gpt2-retired-history
ROOT="$BASE/simulator"
RUN="$BASE/smoke-two-unified-gz"
trap 'rc=$?; printf "%s\n" "$rc" > "$BASE/smoke-compressed-launcher.rc"' EXIT
cd "$BASE"
test "$(cat prepare.rc)" = 0
test ! -e "$RUN"
export GPGPUSIM_ROOT="$ROOT/gpu-simulator/gpgpu-sim"
export ICXT_CFG="$GPGPUSIM_ROOT/configs/tested-cfgs/SM7_QV100/config_volta_islip.icnt"
export REPO_ROOT="$ROOT" CUDA_INSTALL_PATH=/usr/local/cuda CUOBJDUMP_SIM_FILE=jj
export PATH="$CUDA_INSTALL_PATH/bin:$ROOT/.local-deps/root/usr/bin:$PATH"
set +u
source "$ROOT/gpu-simulator/setup_environment.sh" release
set -u
LIBDIR="$(dirname "$(find "$GPGPUSIM_ROOT/lib" -path '*/release/libcudart.so' -print -quit)")"
export LD_LIBRARY_PATH="$LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export HBF_EVENTLAB_GPU_MSHRS=512 RUN_TIMEOUT=1200 GPU_CHANNEL_BW_GBPS=188
ulimit -v 25165824
bash run_unified_replay_compressed.sh \
  "$ROOT/gpu-simulator/bin/release/accel-sim.out" \
  "$OLD/gpt2-sim-v5/gpgpusim.config" \
  "$PREV/trace-two/kernelslist.g" \
  "$OLD/replay/image-pages.txt" "$RUN" \
  "$BASE/config/gpt2-eventlab-device.ini"
test "$(cat "$RUN/run.rc")" = 0
test "$(grep -c '^kernel_launch_uid' "$RUN/run.log")" = 2
test -s "$RUN/backend-input.ini.effective.ini"
gzip -t "$RUN/eventlab.csv.gz"
python3 source/audit_trace.py "$RUN/eventlab.csv.gz" > smoke-compressed-audit.json
python3 - <<'PY'
import json
with open('smoke-compressed-audit.json') as f:
    audit=json.load(f)
assert audit['valid'] and audit['requests']==768
PY
grep '^EVENTLAB_GPU ' "$RUN/run.log" | tail -n 1 | grep -q 'failed=0 owned=0'
grep '^EVENTLAB_DEVICE ' "$RUN/run.log" | tail -n 1 | grep -q 'drained=1'
sha256sum "$RUN/backend-input.ini" "$RUN/backend-input.ini.effective.ini" \
  "$ROOT/gpu-simulator/bin/release/accel-sim.out" > smoke-compressed-source.sha256
