#!/usr/bin/env bash
set -euo pipefail
EVENTLAB_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SIM_ROOT="${1:?usage: smoke_accel_sim_eventlab.sh /path/to/HBF-Sim-eventlab RUN_TAG}"
SIM_ROOT="$(cd "$SIM_ROOT" && pwd)"
TAG="${2:?a fresh RUN_TAG is required}"
[[ "$TAG" =~ ^[A-Za-z0-9_-]+$ ]] || exit 2
OUT="$EVENTLAB_ROOT/research/live-gpu-$TAG"
[[ ! -e "$OUT" ]] || { echo 'Refusing existing results directory' >&2; exit 2; }
BIN="$OUT/bin"
mkdir -p "$BIN"
source "$SIM_ROOT/experiments/common/env.sh"
export PATH="$CUDA_INSTALL_PATH/bin:$PATH"
export PYTHON="${PYTHON:-$SIM_ROOT/.venv/bin/python}"
export RUN_TIMEOUT=300
ulimit -c 0
BASE="$GPGPUSIM_ROOT/configs/tested-cfgs/SM7_QV100/gpgpusim_hbf.config"
for name in channel_probe shared_staging; do
  "$CUDA_INSTALL_PATH/bin/nvcc" -arch=sm_70 --cudart shared \
    "$SIM_ROOT/experiments/workloads/$name.cu" -o "$BIN/$name"
done
"$CUDA_INSTALL_PATH/bin/nvcc" -arch=sm_70 --cudart shared \
  "$EVENTLAB_ROOT/integration/accel_sim/full_page.cu" -o "$BIN/full_page"
make_config() {
  local file="$1" channels="$2" sense="$3"
  sed -e 's/^-gpgpu_n_clusters .*/-gpgpu_n_clusters 8/' \
      -e 's/^-gpgpu_n_mem .*/-gpgpu_n_mem 8/' \
      -e 's/^-gpgpu_gmem_skip_L1D .*/-gpgpu_gmem_skip_L1D 1/' \
      -e 's/^-gpgpu_dram_partition_queues .*/-gpgpu_dram_partition_queues 4096:4096:4096:4096/' \
      -e 's/^-gpgpu_hbf_l2_policy .*/-gpgpu_hbf_l2_policy 0/' \
      -e 's/^-gpgpu_hbf_num_stacks .*/-gpgpu_hbf_num_stacks 1/' \
      -e "s/^-gpgpu_hbf_num_channels .*/-gpgpu_hbf_num_channels $channels/" \
      -e 's/^-gpgpu_hbf_placement_mode .*/-gpgpu_hbf_placement_mode 0/' \
      -e "s/^-gpgpu_hbf_tR .*/-gpgpu_hbf_tR $sense/" \
      -e 's/^-gpgpu_hbf_tPROG .*/-gpgpu_hbf_tPROG 40/' \
      -e 's/^-gpgpu_hbf_tBERS .*/-gpgpu_hbf_tBERS 80/' \
      -e 's/^-gpgpu_hbf_write_agg_timeout .*/-gpgpu_hbf_write_agg_timeout 100000/' \
      -e 's/^-gpgpu_hbf_trace_file .*/-gpgpu_hbf_trace_file ""/' \
      -e 's/^-gpgpu_hbf_trace_level .*/-gpgpu_hbf_trace_level 0/' \
      "$BASE" > "$file"
}
run_case() {
  local name="$1" image="$2" binary="$3";shift 3
  export HBF_BACKEND=eventlab HBF_EVENTLAB_IMAGE_PAGES="$image"
  export HBF_EVENTLAB_TRACE="$OUT/$name.csv"
  ionice -c 2 -n 7 nice -n 10 "$SIM_ROOT/experiments/common/run_sim.sh" \
    "$OUT/$name" "$OUT/$name.config" "$BIN/$binary" "$@"
  "$PYTHON" "$EVENTLAB_ROOT/scripts/check_live_gpu_trace.py" "$OUT/$name.csv" > "$OUT/$name.validation.json"
  grep -Eq 'EVENTLAB_GPU admitted=[1-9][0-9]* completed=[1-9][0-9]* returned=[1-9][0-9]* failed=0 owned=0' "$OUT/$name/run.log"
  grep -Eq 'EVENTLAB_DEVICE senses=[1-9][0-9]* .*drained=1' "$OUT/$name/run.log"
}
for channels in 1 2 4; do
  name="channels_$channels";make_config "$OUT/$name.config" "$channels" 20
  run_case "$name" 4 channel_probe "$HBF_BASE" 4 4096
done
make_config "$OUT/merge.config" 4 20
run_case merge 1 shared_staging "$HBF_BASE" 256
make_config "$OUT/slow.config" 4 200
run_case slow 1 shared_staging "$HBF_BASE" 256
merge_cycles="$(awk '/gpu_sim_cycle =/{value=$3} END{print value}' "$OUT/merge/run.log")"
slow_cycles="$(awk '/gpu_sim_cycle =/{value=$3} END{print value}' "$OUT/slow/run.log")"
[[ "$merge_cycles" =~ ^[0-9]+$ && "$slow_cycles" =~ ^[0-9]+$ && "$slow_cycles" -gt "$merge_cycles" ]] || {
  echo "Device latency did not feed back into GPU completion: merge=$merge_cycles slow=$slow_cycles" >&2; exit 1;
}
make_config "$OUT/full_page.config" 4 20
run_case full_page 0 full_page "$HBF_BASE"
grep -Fq '[FULL-PAGE] 1024 values verified' "$OUT/full_page/run.log"
make_config "$OUT/recovery.config" 4 20
export HBF_EVENTLAB_FAIL_ONCE=1
run_case recovery 1 shared_staging "$HBF_BASE" 256
unset HBF_EVENTLAB_FAIL_ONCE
grep -Eq 'replay_passes=[1-9][0-9]*' "$OUT/recovery/run.log"
echo "Live GPU/EventLab smoke results: $OUT"
