#pragma once
#include "hbf_eventlab/gpu/gpu_issue_gate.hpp"
#include <istream>

namespace hbf_eventlab {
// Format v1: READ/WRITE id gpu_cycle global_byte bytes [key=value ...].
// gpu_cycle is converted once to the EventLab Tick domain with cycle_time.
std::vector<GpuTraceRequest> read_gpu_trace(std::istream&,Tick cycle_time);
} // namespace hbf_eventlab
