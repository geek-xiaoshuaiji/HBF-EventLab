#!/usr/bin/env bash
# Build only in a separate HBF-Sim checkout, never in the verified reference.
set -euo pipefail
EVENTLAB_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INTEGRATION_ROOT="${1:?usage: build_accel_sim_eventlab.sh /path/to/HBF-Sim-eventlab}"
INTEGRATION_ROOT="$(cd "$INTEGRATION_ROOT" && pwd)"
GPU_ROOT="$INTEGRATION_ROOT/gpu-simulator/gpgpu-sim"
JOBS="${JOBS:-2}"
[[ "$JOBS" =~ ^[1-4]$ ]] || { echo 'JOBS must be 1..4' >&2; exit 2; }
[[ "$(basename "$INTEGRATION_ROOT")" == HBF-Sim-eventlab ]] || {
  echo 'Refusing to modify a reference tree: use a separate HBF-Sim-eventlab directory' >&2; exit 2;
}
[[ "$(git -C "$INTEGRATION_ROOT" rev-parse HEAD)" == 3415a3171d9312fac6a835bc1aa188f7aab685c0 ]] || exit 2
[[ "$(git -C "$GPU_ROOT" rev-parse HEAD)" == 6c3cf4ff32110908386d605a7034fc67666a92de ]] || exit 2
ADAPTER="$EVENTLAB_ROOT/integration/accel_sim"
integration_complete() {
  grep -q 'hbf_eventlab_bridge_t \*m_eventlab' "$GPU_ROOT/src/gpgpu-sim/hbf_system.h" &&
  grep -q 'hbf_system_t::cycle_at(double dram_time_seconds)' "$GPU_ROOT/src/gpgpu-sim/hbf_system.cc" &&
  grep -q 'm_eventlab->pop_return_for' "$GPU_ROOT/src/gpgpu-sim/hbf_system.cc" &&
  grep -q 'm_hbf_system->cycle_at' "$GPU_ROOT/src/gpgpu-sim/gpu-sim.cc" &&
  grep -q 'HBF_EVENTLAB_ROOT' "$GPU_ROOT/src/gpgpu-sim/Makefile" &&
  grep -q 'HBF_EVENTLAB_LIBRARY' "$GPU_ROOT/Makefile"
}
if ! integration_complete; then
  if grep -q 'hbf_eventlab_bridge_t \*m_eventlab\|m_hbf_system->cycle_at\|HBF_EVENTLAB_LIBRARY' \
      "$GPU_ROOT/src/gpgpu-sim/hbf_system.h" "$GPU_ROOT/src/gpgpu-sim/gpu-sim.cc" "$GPU_ROOT/Makefile"; then
    echo 'Partial EventLab patch detected; restore the five pinned upstream files before retrying' >&2
    exit 3
  fi
  patch --fuzz=0 --dry-run -d "$GPU_ROOT" -p3 < "$ADAPTER/eventlab_backend.patch"
  patch --fuzz=0 -d "$GPU_ROOT" -p3 < "$ADAPTER/eventlab_backend.patch"
fi
integration_complete
cp "$ADAPTER/hbf_eventlab_bridge.h" "$ADAPTER/hbf_eventlab_bridge.cc" "$GPU_ROOT/src/gpgpu-sim/"
export HBF_EVENTLAB_ROOT="$EVENTLAB_ROOT"
export HBF_EVENTLAB_LIBRARY="$EVENTLAB_ROOT/build-gpu-pic/libhbf_eventlab.a"
cmake -S "$EVENTLAB_ROOT" -B "$EVENTLAB_ROOT/build-gpu-pic" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build "$EVENTLAB_ROOT/build-gpu-pic" -j "$JOBS"
export CUDA_INSTALL_PATH="${CUDA_INSTALL_PATH:-/usr/local/cuda}"
export PATH="$CUDA_INSTALL_PATH/bin:$INTEGRATION_ROOT/.local-deps/root/usr/bin:$PATH"
# Use the upstream environment but do not weaken its reference verifier.
# This intentionally modified variant has its own patch and provenance.
set +u
source "$INTEGRATION_ROOT/gpu-simulator/setup_environment.sh" release
set -u
make -j"$JOBS" -C "$INTEGRATION_ROOT/gpu-simulator"
git -C "$GPU_ROOT" diff --check
sha256sum "$ADAPTER/"* "$HBF_EVENTLAB_LIBRARY"
