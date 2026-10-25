#include "hbf_eventlab/adapters/gpu_trace_reader.hpp"
#include "hbf_eventlab/runtime/simulation_runner.hpp"
#include "hbf_eventlab/policies/round_robin_interleaver.hpp"
#include <iostream>
#include <sstream>

using namespace hbf_eventlab;
namespace {
std::size_t checks=0;
void check(bool b,const char* why){++checks;if(!b)throw std::runtime_error(why);}
template<class F> void rejects(F f){bool yes=false;try{f();}catch(const std::exception&){yes=true;}check(yes,"expected rejection");}
std::vector<GpuTraceRequest> sample(){
    std::istringstream in("HBF_EVENTLAB_GPU_TRACE 1\n"
        "WRITE 1 0 0 4096 content=1 source=0\n"
        "READ 2 10 0 64 after=1 expect=1 source=3\n");
    return read_gpu_trace(in,Tick{500});
}
void parser(){
    const auto v=sample();check(v.size()==2,"two requests");
    check(v[1].request.not_before==Tick{5000}&&v[1].source==3,"cycle conversion and source");
    check(v[1].request.after==std::vector<GlobalRequestId>{GlobalRequestId{1}},"dependency");
    rejects([]{std::istringstream in("HBF_EVENTLAB_GPU_TRACE 2\n");read_gpu_trace(in,Tick{1});});
    rejects([]{std::istringstream in("HBF_EVENTLAB_GPU_TRACE 1\nWRITE 1 0 0 64\n");read_gpu_trace(in,Tick{1});});
}
GpuTraceRequest read(std::uint64_t id,std::uint64_t address){
    GpuTraceRequest item;item.request={GlobalRequestId{id},Op::Read,GlobalByteAddress{address},64};return item;
}
void issue_window(){
    std::vector<GpuTraceRequest> v{read(1,0),read(2,4096),read(3,64)};GpuIssueGate gate(v,2,1);
    check(gate.can_issue({GlobalRequestId{1}}),"first fits");gate.issued({GlobalRequestId{1}},Tick{});
    check(!gate.can_issue({GlobalRequestId{2}}),"different page blocked by MSHR");
    check(gate.can_issue({GlobalRequestId{3}}),"same page merges");gate.issued({GlobalRequestId{3}},Tick{});
    check(gate.statistics().mshr_misses==1&&gate.statistics().mshr_hits==1,"MSHR accounting");
    WorkResult done1{GlobalRequestId{1},LogicalResult::Success};gate.completed(done1,Tick{10});
    check(!gate.can_issue({GlobalRequestId{2}}),"shared page remains until all users finish");
    WorkResult done3{GlobalRequestId{3},LogicalResult::Success};gate.completed(done3,Tick{10});
    check(gate.can_issue({GlobalRequestId{2}}),"completion releases MSHR");
}
void closed_loop(){
    auto trace=sample();auto gate=std::make_shared<GpuIssueGate>(trace,1,1);
    Geometry geometry{1,1,1,8,4,1};StaticDeviceDescription desc{geometry,{256,8,false,true},{256,Tick{10000000},geometry.block_group_dlus()}};
    EventQueue events;DeviceDriver device(desc,{},events);RoundRobinInterleaver inter({},1,geometry.channel_bytes());
    HostDriver host(device,events,inter,{},std::make_unique<FillOpenDluPolicy>(),gate);
    std::vector<GlobalRequest> workload;for(const auto& item:trace)workload.push_back(item.request);
    host.load(workload);host.start();SimulationRunner runner(events,host,device);const auto report=runner.run();
    check(report.status==RunStatus::Completed&&report.workload_success,"closed loop succeeds");
    check(gate->statistics().issued_requests==2&&gate->statistics().completed_requests==2,"GPU requests observed");
    check(gate->statistics().max_outstanding==1,"issue limit enforced by completion feedback");
    check(gate->activity().outstanding==0&&gate->activity().active_mshrs==0,"GPU state drained");
}
}
int main(int argc,char**argv){
    try{
        const std::string scenario=argc==2?argv[1]:"";
        if(scenario=="parser")parser();else if(scenario=="issue_window")issue_window();else if(scenario=="closed_loop")closed_loop();
        else throw std::invalid_argument("unknown scenario");
        std::cout<<"PASS gpu_"<<scenario<<" ("<<checks<<" checks)\n";return 0;
    }catch(const std::exception&e){std::cerr<<"FAIL gpu: "<<e.what()<<"\n";return 1;}
}
