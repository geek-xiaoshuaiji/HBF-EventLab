#pragma once
#include "hbf_eventlab/interfaces/global_request_gate.hpp"
#include <map>
#include <set>
#include <bitset>

namespace hbf_eventlab {
struct GpuTraceRequest {
    GlobalRequest request;
    std::uint64_t gpu_cycle = 0;
    std::uint32_t source = 0;
    // Live GPU masks are relative to the enclosing 128B access window.
    std::bitset<128> byte_mask;
};

struct GpuIssueStatistics {
    std::uint64_t trace_requests = 0;
    std::uint64_t issued_requests = 0;
    std::uint64_t completed_requests = 0;
    std::uint64_t completed_unissued = 0;
    std::uint64_t mshr_hits = 0;
    std::uint64_t mshr_misses = 0;
    std::uint64_t max_outstanding = 0;
    std::uint64_t max_mshr_occupancy = 0;
    std::uint64_t total_issue_delay = 0;
    std::uint64_t max_issue_delay = 0;
    std::uint64_t total_latency = 0;
    std::uint64_t max_latency = 0;
};

struct GpuIssueActivity {
    std::size_t outstanding = 0;
    std::size_t active_mshrs = 0;
};

// Models a finite GPU request window and page-keyed MSHR occupancy. It does
// not change HBF command granularity: cache-line requests still become 64B
// Host work and page service remains owned by the HBF read path/Bank Cache.
class GpuIssueGate final : public IGlobalRequestGate {
public:
    GpuIssueGate(const std::vector<GpuTraceRequest>&, std::size_t issue_limit, std::size_t mshr_entries);
    bool can_issue(const std::vector<GlobalRequestId>&) const override;
    void issued(const std::vector<GlobalRequestId>&, Tick) override;
    void completed(const WorkResult&, Tick) override;
    const GpuIssueStatistics& statistics() const { return stats_; }
    GpuIssueActivity activity() const { return {active_.size(), page_references_.size()}; }
    std::size_t issue_limit() const { return issue_limit_; }
    std::size_t mshr_entries() const { return mshr_entries_; }
private:
    struct RequestInfo { Tick ready; std::vector<std::uint64_t> pages; };
    std::vector<GlobalRequestId> new_requests(const std::vector<GlobalRequestId>&) const;
    const RequestInfo& info(GlobalRequestId) const;
    std::map<GlobalRequestId,RequestInfo> requests_;
    std::set<GlobalRequestId> active_,completed_;
    std::map<std::uint64_t,std::size_t> page_references_;
    std::size_t issue_limit_,mshr_entries_;
    GpuIssueStatistics stats_;
};
} // namespace hbf_eventlab
