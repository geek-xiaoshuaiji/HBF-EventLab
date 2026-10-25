#pragma once
#include "hbf_eventlab/host/workload.hpp"
#include <vector>

namespace hbf_eventlab {
// Optional source-side admission seam. The Host still owns command splitting,
// retries and recovery; a gate only controls when a Global request may enter
// the Host/Device feedback loop and observes its final logical completion.
class IGlobalRequestGate {
public:
    virtual ~IGlobalRequestGate() = default;
    virtual bool can_issue(const std::vector<GlobalRequestId>&) const = 0;
    virtual void issued(const std::vector<GlobalRequestId>&, Tick) = 0;
    virtual void completed(const WorkResult&, Tick) = 0;
};
} // namespace hbf_eventlab
