#include "hbf_eventlab/adapters/gpu_trace_reader.hpp"
#include "hbf_eventlab/gpu/page_controller.hpp"
#include "hbf_eventlab/gpu/page_interleaver.hpp"
#include "hbf_eventlab/runtime/simulation_runner.hpp"
#include <fstream>
#include <iostream>

using namespace hbf_eventlab;
namespace {
std::uint64_t number(const std::string& s){
    if(s.empty()||s[0]=='-'||s[0]=='+')throw std::invalid_argument("expected unsigned number");
    std::size_t end=0;auto n=std::stoull(s,&end,s.rfind("0x",0)==0||s.rfind("0X",0)==0?16:10);
    if(end!=s.size())throw std::invalid_argument("invalid number");return n;
}
unsigned small(const std::string& s){auto n=number(s);if(n>UINT32_MAX)throw std::invalid_argument("integer too large");return static_cast<unsigned>(n);}
}
int main(int argc,char** argv){
    try {
        if(argc<2){std::cerr<<"usage: hbf_eventlab_gpu_cli trace.txt [--issue-limit N] [--issue-width N] [--mshrs N]\n"
            <<" [--gpu-cycle-ps N] [--stacks N] [--channels N] [--dies N] [--banks N]\n"
            <<" [--mapping interleave|contiguous|grouped] [--group-pages N] [--read-window N] [--no-merge]\n"
            <<" [--write-buffers N] [--write-deadline N] [--fail-page N] [--max-tick N]\n";return 5;}
        GpuControllerConfiguration gc;unsigned stacks=1,channels=2,dies=1,banks=1;
        std::uint64_t group_pages=2;GpuPagePlacement placement=GpuPagePlacement::Interleave;
        std::optional<PageId> fail_page;RunLimits limits;HostConfiguration hc;ExecutionConfiguration ec;
        for(int i=2;i<argc;++i){
            std::string key=argv[i];if(key=="--no-merge"){gc.merge_reads=false;continue;}
            if(i+1>=argc)throw std::invalid_argument("missing option value: "+key);std::string value=argv[++i];
            if(key=="--issue-limit")gc.issue_limit=number(value);
            else if(key=="--issue-width")gc.issue_width=number(value);
            else if(key=="--mshrs")gc.mshrs=number(value);
            else if(key=="--gpu-cycle-ps")gc.cycle=Tick{number(value)};
            else if(key=="--stacks")stacks=small(value);
            else if(key=="--channels")channels=small(value);
            else if(key=="--dies")dies=small(value);
            else if(key=="--banks")banks=small(value);
            else if(key=="--group-pages")group_pages=number(value);
            else if(key=="--write-buffers")gc.write_buffers=number(value);
            else if(key=="--write-deadline")gc.write_deadline=Tick{number(value)};
            else if(key=="--read-window")gc.read_window=Tick{number(value)};
            else if(key=="--fail-page")fail_page=small(value);
            else if(key=="--max-tick")limits.max_tick=Tick{number(value)};
            else if(key=="--mapping"){
                if(value=="interleave")placement=GpuPagePlacement::Interleave;
                else if(value=="contiguous")placement=GpuPagePlacement::Contiguous;
                else if(value=="grouped")placement=GpuPagePlacement::Grouped;
                else throw std::invalid_argument("unknown mapping");
            }else throw std::invalid_argument("unknown option: "+key);
        }
        const auto total_channels=checked_mul(stacks,channels);if(!total_channels||total_channels>UINT32_MAX)throw std::invalid_argument("invalid stack/channel count");
        Geometry geometry{static_cast<unsigned>(total_channels),dies,banks,8,4,1};
        if(fail_page&&*fail_page>=geometry.pages_per_block)throw std::invalid_argument("fault page outside block");
        StaticDeviceDescription desc{geometry,{256,8,false,true},{256,Tick{10000000},geometry.block_group_dlus()}};
        EventQueue events;DeviceDriver device(desc,ec,events);
        if(fail_page)device.set_program_outcome([target=*fail_page,used=false](const ProgramOperation& op)mutable{
            if(!used&&op.logical.channel==0&&op.physical.page==target){used=true;return ProgramOutcome::FailedReusable;}
            return ProgramOutcome::Success;
        });
        GpuPageInterleaver inter(static_cast<unsigned>(total_channels),geometry.channel_bytes(),placement,group_pages);
        HostDriver host(device,events,inter,hc);host.load({});host.start();
        std::ifstream input(argv[1]);if(!input)throw std::invalid_argument("cannot open GPU trace file");
        GpuPageController gpu(host,events,read_gpu_trace(input,gc.cycle),gc);gpu.start();
        SimulationRunner runner(events,host,device);
        runner.set_source_probes([&]{return gpu.drained();},[&]{return gpu.success();},[&]{return gpu.waiting_reason();});
        const auto report=runner.run(limits);const auto& g=gpu.statistics();const auto& h=host.statistics();const auto& d=device.statistics();
        std::cout<<"HBF EventLab + HBF-Sim helper import | trace-driven GPU request simulation\n"
            <<"GPU_CONFIG stacks="<<stacks<<" channels_per_stack="<<channels<<" dies="<<dies<<" banks="<<banks
            <<" cycle_ps="<<gc.cycle.value<<" issue_width="<<gc.issue_width<<" issue_limit="<<gc.issue_limit<<" mshrs="<<gc.mshrs<<"\n"
            <<"STOP "<<to_string(report.status)<<" workload="<<(report.workload_success?"Success":"FailedOrUnfinished")
            <<" tick="<<report.at.value<<" events="<<report.events<<" reason="<<report.explanation<<"\n"
            <<"GPU issued="<<g.issued<<" completed="<<g.completed<<" failed="<<g.failed<<" max_outstanding="<<g.max_outstanding
            <<" mshr_hits="<<g.mshr_hits<<" page_reads="<<g.page_reads<<" page_writes="<<g.page_writes<<" max_mshrs="<<g.max_mshrs
            <<" incomplete_writes="<<g.incomplete_writes<<" unique_write_bytes="<<g.unique_write_bytes
            <<" requested_read_bytes="<<g.requested_read_bytes<<" page_read_bytes="<<g.media_read_bytes<<"\n"
            <<"HOST handed="<<h.handed<<" responses="<<h.responses<<" attempt_errors="<<h.attempt_errors
            <<" replay_passes="<<h.replay_passes<<" logical_failed="<<h.logical_failed<<"\n"
            <<"DEVICE programs="<<d.programs<<" senses="<<d.senses<<" cache_hits="<<d.cache_hits
            <<" gpu_drained="<<gpu.drained()<<" host_drained="<<report.host.drained()<<" device_drained="<<report.device.drained()<<"\n";
        for(const auto& item:gpu.results())if(!item.second.success)
            std::cout<<"GPU_ERROR id="<<item.first.value<<" source="<<item.second.source<<" reason="<<item.second.reason<<"\n";
        if(report.status==RunStatus::Completed)return report.workload_success?0:2;
        return report.status==RunStatus::Stalled?3:report.status==RunStatus::LimitReached?4:report.status==RunStatus::ModelError?5:6;
    }catch(const std::exception& e){std::cerr<<"INPUT/MODEL ERROR: "<<e.what()<<"\n";return 5;}
}
