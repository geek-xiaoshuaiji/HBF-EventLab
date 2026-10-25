#include "hbf_eventlab/gpu/gpu_issue_gate.hpp"
#include <algorithm>

namespace hbf_eventlab {
namespace {
std::vector<std::uint64_t> pages_for(const GlobalRequest& request) {
    if(!request.bytes) throw std::invalid_argument("GPU request must not be empty");
    const auto last=checked_add(request.address.value,request.bytes-1);
    std::vector<std::uint64_t> pages;
    for(auto page=request.address.value/dlu_bytes;page<=last/dlu_bytes;++page)pages.push_back(page);
    return pages;
}
}
GpuIssueGate::GpuIssueGate(const std::vector<GpuTraceRequest>& input,std::size_t issue_limit,std::size_t mshr_entries)
    :issue_limit_(issue_limit),mshr_entries_(mshr_entries) {
    if(!issue_limit_||!mshr_entries_)throw std::invalid_argument("GPU issue limit and MSHR entries must be positive");
    stats_.trace_requests=input.size();
    for(const auto& item:input) {
        auto pages=pages_for(item.request);
        if(pages.size()>mshr_entries_)throw std::invalid_argument("one GPU request spans more pages than the MSHR capacity");
        if(!requests_.emplace(item.request.id,RequestInfo{item.request.not_before,std::move(pages)}).second)
            throw std::invalid_argument("duplicate GPU request ID");
    }
}
const GpuIssueGate::RequestInfo& GpuIssueGate::info(GlobalRequestId id) const {
    const auto it=requests_.find(id);if(it==requests_.end())throw std::logic_error("unknown GPU request ID");return it->second;
}
std::vector<GlobalRequestId> GpuIssueGate::new_requests(const std::vector<GlobalRequestId>& ids) const {
    std::set<GlobalRequestId> unique;std::vector<GlobalRequestId> out;
    for(auto id:ids) {
        (void)info(id);
        if(active_.count(id)||completed_.count(id)||!unique.insert(id).second)continue;
        out.push_back(id);
    }
    return out;
}
bool GpuIssueGate::can_issue(const std::vector<GlobalRequestId>& ids) const {
    const auto fresh=new_requests(ids);
    if(active_.size()+fresh.size()>issue_limit_)return false;
    std::set<std::uint64_t> new_pages;
    for(auto id:fresh)for(auto page:info(id).pages)if(!page_references_.count(page))new_pages.insert(page);
    return page_references_.size()+new_pages.size()<=mshr_entries_;
}
void GpuIssueGate::issued(const std::vector<GlobalRequestId>& ids,Tick at) {
    const auto fresh=new_requests(ids);
    if(active_.size()+fresh.size()>issue_limit_)throw std::logic_error("GPU issue window overcommit");
    std::set<std::uint64_t> new_pages;
    for(auto id:fresh)for(auto page:info(id).pages)if(!page_references_.count(page))new_pages.insert(page);
    if(page_references_.size()+new_pages.size()>mshr_entries_)throw std::logic_error("GPU MSHR overcommit");
    for(auto id:fresh) {
        const auto& request=info(id);
        if(at<request.ready)throw std::logic_error("GPU request issued before ready Tick");
        active_.insert(id);++stats_.issued_requests;
        const auto delay=at.value-request.ready.value;
        stats_.total_issue_delay=checked_add(stats_.total_issue_delay,delay);
        stats_.max_issue_delay=std::max(stats_.max_issue_delay,delay);
        for(auto page:request.pages) {
            auto& references=page_references_[page];
            if(references)++stats_.mshr_hits;else ++stats_.mshr_misses;
            ++references;
        }
    }
    stats_.max_outstanding=std::max<std::uint64_t>(stats_.max_outstanding,active_.size());
    stats_.max_mshr_occupancy=std::max<std::uint64_t>(stats_.max_mshr_occupancy,page_references_.size());
}
void GpuIssueGate::completed(const WorkResult& result,Tick at) {
    if(!completed_.insert(result.id).second)throw std::logic_error("GPU request completed twice");
    ++stats_.completed_requests;
    const auto& request=info(result.id);
    if(at<request.ready)throw std::logic_error("GPU request completed before ready Tick");
    const auto latency=at.value-request.ready.value;
    stats_.total_latency=checked_add(stats_.total_latency,latency);
    stats_.max_latency=std::max(stats_.max_latency,latency);
    if(!active_.erase(result.id)) {++stats_.completed_unissued;return;}
    for(auto page:request.pages) {
        auto it=page_references_.find(page);
        if(it==page_references_.end()||!it->second)throw std::logic_error("GPU MSHR accounting underflow");
        if(--it->second==0)page_references_.erase(it);
    }
}
} // namespace hbf_eventlab
