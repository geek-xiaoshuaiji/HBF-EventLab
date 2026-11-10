#!/usr/bin/env bash
set -euo pipefail
BASE=/home/server/hbf-experiments/20261109-gpt2-unified
ROOT="$BASE/simulator"
trap 'rc=$?; printf "%s\n" "$rc" > "$BASE/prepare.rc"' EXIT
cd "$BASE"
test ! -e prepare.rc
mkdir -p tmp
export TMPDIR="$BASE/tmp"
printf '%s\n' d32959113553ed9b38eb02bb10f3b85cb01f74e5 > upstream-commit.txt
sha256sum source.zip source/hbf_eventlab_bridge.cc \
  gpu-headers/hbf_eventlab/gpu/configured_backend.hpp \
  config/gpt2-eventlab-device.ini source/run_unified_replay.sh > input-source.sha256

cmake -S source -B build-gpu -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON > cmake.log 2>&1
cmake --build build-gpu -j2 > build-gpu.log 2>&1
ctest --test-dir build-gpu -j2 --output-on-failure > ctest.log 2>&1

cp source/hbf_eventlab_bridge.cc \
  "$ROOT/gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf_eventlab_bridge.cc"
export HBF_EVENTLAB_ROOT="$BASE/EventLab"
export HBF_EVENTLAB_LIBRARY="$BASE/build-gpu/backend/libhbf_eventlab.a"
export CPLUS_INCLUDE_PATH="$BASE/gpu-headers:$BASE/adapter/third_party/hbf_sim"
export CUDA_INSTALL_PATH=/usr/local/cuda
export PATH="$CUDA_INSTALL_PATH/bin:$ROOT/.local-deps/root/usr/bin:$PATH"
set +u
source "$ROOT/gpu-simulator/setup_environment.sh" release
set -u
make -j2 -C "$ROOT/gpu-simulator" > build-simulator.log 2>&1
sha256sum "$ROOT/gpu-simulator/bin/release/accel-sim.out" \
  "$HBF_EVENTLAB_LIBRARY" > binaries.sha256
