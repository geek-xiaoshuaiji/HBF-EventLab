#!/usr/bin/env bash
set -euo pipefail
BASE=/home/server/hbf-experiments/20261109-gpt2-unified
OLD=/home/server/hbf-experiments/20261028-gpt2-incremental
PREV=/home/server/hbf-experiments/20261107-gpt2-retired-history
ROOT="$BASE/simulator"
RUN="$BASE/full-run-unified-gz"
trap 'rc=$?; printf "%s\n" "$rc" > "$BASE/full-compressed-launcher.rc"' EXIT
cd "$BASE"
test "$(cat prepare.rc)" = 0
test "$(cat smoke-launcher.rc)" = 0
test "$(cat smoke-two-unified/run.rc)" = 0
test "$(cat smoke-compressed-launcher.rc)" = 0
test "$(cat smoke-two-unified-gz/run.rc)" = 0
test ! -e "$RUN"
sha256sum -c input-source.sha256
sha256sum -c binaries.sha256
python3 source/audit_trace.py smoke-two-unified/eventlab.csv > smoke-audit.json
python3 - <<'PY'
import json
with open('smoke-audit.json') as f:
    audit=json.load(f)
assert audit['valid'] and audit['requests']==768
PY
test -f "$OLD/replay/kernelslist.g"
test -f "$OLD/replay/image-pages.txt"
test -f "$OLD/gpt2-sim-v5/gpgpusim.config"
sha256sum source/hbf_eventlab_bridge.cc source/audit_trace.py \
  source/collect_metrics.py run_unified_replay_compressed.sh \
  gpu-headers/hbf_eventlab/gpu/configured_backend.hpp \
  config/gpt2-eventlab-device.ini source/run_unified_replay.sh \
  "$ROOT/gpu-simulator/bin/release/accel-sim.out" \
  "$OLD/replay/kernelslist.g" "$OLD/replay/image-pages.txt" \
  "$OLD/gpt2-sim-v5/gpgpusim.config" > full-compressed-source.sha256

export GPGPUSIM_ROOT="$ROOT/gpu-simulator/gpgpu-sim"
export ICXT_CFG="$GPGPUSIM_ROOT/configs/tested-cfgs/SM7_QV100/config_volta_islip.icnt"
export REPO_ROOT="$ROOT" CUDA_INSTALL_PATH=/usr/local/cuda CUOBJDUMP_SIM_FILE=jj
export PATH="$CUDA_INSTALL_PATH/bin:$ROOT/.local-deps/root/usr/bin:$PATH"
set +u
source "$ROOT/gpu-simulator/setup_environment.sh" release
set -u
LIBDIR="$(dirname "$(find "$GPGPUSIM_ROOT/lib" -path '*/release/libcudart.so' -print -quit)")"
export LD_LIBRARY_PATH="$LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export HBF_EVENTLAB_GPU_MSHRS=512 RUN_TIMEOUT=172800 GPU_CHANNEL_BW_GBPS=188
ulimit -v 25165824
date -u '+start_utc=%Y-%m-%dT%H:%M:%SZ' > full-run-compressed.time
bash run_unified_replay_compressed.sh \
  "$ROOT/gpu-simulator/bin/release/accel-sim.out" \
  "$OLD/gpt2-sim-v5/gpgpusim.config" \
  "$OLD/replay/kernelslist.g" \
  "$OLD/replay/image-pages.txt" "$RUN" \
  "$BASE/config/gpt2-eventlab-device.ini" > full-run-compressed-launcher.log 2>&1
date -u '+simulation_end_utc=%Y-%m-%dT%H:%M:%SZ' >> full-run-compressed.time
test "$(cat "$RUN/run.rc")" = 0
python3 source/collect_metrics.py --capture "$OLD/capture-v2" \
  --replay "$OLD/replay" --run "$RUN" --reference "$OLD/native-reference" \
  --output "$BASE/summary.json" > summary.log 2>&1
python3 - <<'PY'
import json
from pathlib import Path
current=json.loads(Path('summary.json').read_text())
baseline=json.loads(Path('/home/server/hbf-experiments/20261107-gpt2-retired-history/summary.json').read_text())
keys=('ttft_ms','tpot_ms','total_gpu_cycles','kernel_count')
comparison={key:{'old':baseline[key],'new':current[key],
                 'delta':current[key]-baseline[key]} for key in keys}
comparison['per_token_end_cycle']=[{'old':a['end_cycle'],'new':b['end_cycle'],
                                    'delta':b['end_cycle']-a['end_cycle']}
                                   for a,b in zip(baseline['per_token'],current['per_token'])]
comparison['same_token_ids']=[x['token_id'] for x in baseline['per_token']]==[x['token_id'] for x in current['per_token']]
comparison['exact_timing_match']=all(comparison[key]['delta']==0 for key in keys)
comparison['source_commit']='d32959113553ed9b38eb02bb10f3b85cb01f74e5'
Path('comparison.json').write_text(json.dumps(comparison,indent=2)+'\n')
PY
date -u '+validated_end_utc=%Y-%m-%dT%H:%M:%SZ' >> full-run-compressed.time
