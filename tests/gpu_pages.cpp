#include "hbf_eventlab/gpu/page_controller.hpp"
#include "hbf_eventlab/gpu/page_interleaver.hpp"
#include "hbf_eventlab/runtime/simulation_runner.hpp"
#include <iostream>

using namespace hbf_eventlab;
namespace {
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
GpuTraceRequest request(std::uint64_t id,Op op,std::uint64_t address,std::uint64_t bytes=64,
                        std::vector<GlobalRequestId> after={}) {
    GpuTraceRequest r;r.request={GlobalRequestId{id},op,GlobalByteAddress{address},bytes,1,{},std::move(after)};
    if(op==Op::Read)r.request.expected_content=1;return r;
}
struct Fixture {
    EventQueue events;DeviceDriver device;GpuPageInterleaver inter;HostDriver host;
    static StaticDeviceDescription profile(unsigned channels,unsigned dies,unsigned banks){
        Geometry g{channels,dies,banks,8,4,1};return {g,{256,8,false,true},{256,Tick{10000000},g.block_group_dlus()}};
    }
    Fixture(unsigned channels=1,unsigned dies=1,unsigned banks=1)
        :device(profile(channels,dies,banks),{},events),inter(channels,device.description().geometry().channel_bytes()),host(device,events,inter){host.load({});host.start();}
    RunReport run(GpuPageController& gpu){gpu.start();SimulationRunner runner(events,host,device);
        runner.set_source_probes([&]{return gpu.drained();},[&]{return gpu.success();},[&]{return gpu.waiting_reason();});return runner.run();}
};
std::vector<GpuTraceRequest> reads(unsigned n){
    std::vector<GpuTraceRequest> v{request(1,Op::Write,0,4096)};
    for(unsigned i=0;i<n;++i)v.push_back(request(i+2,Op::Read,i*64,64,{GlobalRequestId{1}}));return v;
}
void merge(){Fixture f;GpuControllerConfiguration c;c.issue_width=8;
    GpuPageController gpu(f.host,f.events,reads(32),c);auto r=f.run(gpu);
    check(r.status==RunStatus::Completed&&r.workload_success,"page merge completes");
    check(gpu.statistics().mshr_hits==31&&gpu.statistics().page_reads==1,"32 GPU lines share one backend page transaction");
    check(f.device.statistics().senses==1,"one physical sense");
    check(gpu.results().size()==33&&gpu.drained(),"all source IDs completed exactly once");
}
void write_coverage(){Fixture f;std::vector<GpuTraceRequest> trace;std::vector<GlobalRequestId> after;
    trace.push_back(request(1,Op::Write,0));after.push_back(GlobalRequestId{1});
    trace.push_back(request(2,Op::Write,0));after.push_back(GlobalRequestId{2});
    for(unsigned i=1;i<64;++i){const auto id=i+2;trace.push_back(request(id,Op::Write,i*64));after.push_back(GlobalRequestId{id});}
    trace.push_back(request(100,Op::Read,0,64,after));GpuControllerConfiguration c;c.issue_width=8;
    GpuPageController gpu(f.host,f.events,std::move(trace),c);const auto r=f.run(gpu);
    check(r.workload_success&&gpu.statistics().unique_write_bytes==4096,"duplicate stores do not increase coverage");
    check(gpu.statistics().page_writes==1&&f.device.statistics().programs==1,"one full DLU programmed");
    const auto at=gpu.results().at(GlobalRequestId{1}).completed;
    for(auto id:after)check(gpu.results().at(id).completed==at,"write replies occur together after backend program");
}
void partial(){Fixture f;GpuControllerConfiguration c;c.write_deadline=Tick{100};
    GpuPageController gpu(f.host,f.events,{request(1,Op::Write,0)},c);auto r=f.run(gpu);
    check(r.status==RunStatus::Completed&&!r.workload_success,"strict incomplete page gives a final failure");
    check(gpu.statistics().incomplete_writes==1&&f.device.statistics().programs==0,"no fabricated padding/program");
    check(gpu.drained()&&r.host.drained()&&r.device.drained(),"all actors drain on strict failure");
}
void feedback(){GpuControllerConfiguration low;low.issue_limit=1;low.issue_width=8;
    Fixture a;GpuPageController ga(a.host,a.events,reads(16),low);auto ra=a.run(ga);
    Fixture b;auto high=low;high.issue_limit=32;GpuPageController gb(b.host,b.events,reads(16),high);auto rb=b.run(gb);
    check(ra.workload_success&&rb.workload_success,"both pressure configurations complete");
    check(ga.statistics().max_outstanding==1&&gb.statistics().max_outstanding>1,"completion controls GPU issue");
    check(ga.statistics().page_reads==16&&gb.statistics().page_reads==1,"issue pressure changes actual page work");
    check(rb.at<ra.at,"merging plus feedback reduces completion window");
}
void multichannel(){Fixture f(4,2,2);std::vector<GpuTraceRequest> trace;
    for(unsigned i=0;i<16;++i)trace.push_back(request(i+1,Op::Write,i*4096,4096));
    GpuControllerConfiguration c;c.mshrs=2;c.issue_width=4;
    GpuPageController gpu(f.host,f.events,std::move(trace),c);auto r=f.run(gpu);
    check(r.workload_success&&f.device.statistics().programs==16,"four-channel die/bank writes complete");
    check(gpu.statistics().max_mshrs<=2,"finite MSHR respected across channels");
    GpuPageInterleaver map(4,32*4096);
    for(unsigned i=0;i<16;++i){auto a=map.map(GlobalByteAddress{i*4096+64});check(a.channel==i%4&&a.local.value==(i/4)*4096+64,"page-affine placement");}
}
void recovery(){Fixture f;f.device.set_program_outcome([failed=false](const ProgramOperation&)mutable{
        if(!failed){failed=true;return ProgramOutcome::FailedReusable;}return ProgramOutcome::Success;});
    GpuPageController gpu(f.host,f.events,reads(4));auto r=f.run(gpu);
    check(r.workload_success&&f.host.statistics().replay_passes==1,"Host replay resolves GPU write");
    check(gpu.statistics().failed==0&&gpu.statistics().completed==5,"GPU sees one final success per request");
}
void issue_width(){Fixture f;GpuControllerConfiguration c;c.issue_width=2;c.cycle=Tick{1000};
    GpuPageController gpu(f.host,f.events,reads(8),c);auto r=f.run(gpu);check(r.workload_success,"issue test completes");
    std::map<std::uint64_t,unsigned> counts;
    for(const auto& item:gpu.results())++counts[item.second.issued.value/1000];
    for(auto count:counts)check(count.second<=2,"at most two GPU requests each cycle");
}
void external_clock(){
    EventQueue q;bool completed=false;
    q.schedule_at(Tick{100},EventPhase::Completion,[&]{completed=true;});
    q.run_until(Tick{99});check(!completed&&q.now()==Tick{99},"future completion must not run early");
    q.run_until(Tick{100});check(completed&&q.pending()==0,"completion runs at horizon");
    bool rejected=false;try{q.run_until(Tick{99});}catch(const std::invalid_argument&){rejected=true;}
    check(rejected,"clock rewind rejected");
    q.run_until(Tick{1000});check(q.now()==Tick{1000},"idle external time advances");
}
void streaming(){Fixture f;GpuControllerConfiguration c;c.sector_requests=true;
    GpuPageController gpu(f.host,f.events,{},c);gpu.start();
    unsigned notices=0;gpu.set_result_observer([&](GlobalRequestId,const GpuRequestResult& r){check(r.success,"live completion success");++notices;});
    auto w=request(1,Op::Write,0,4096);w.request.not_before=f.events.now();gpu.enqueue(w);
    auto read=request(2,Op::Read,32,32,{GlobalRequestId{1}});gpu.enqueue(read);
    f.events.run_until(Tick{1});check(notices==0,"live requests cannot complete before NAND");
    f.events.run();check(notices==2&&gpu.drained(),"live write/read drains");
    auto later=request(3,Op::Read,96,32);later.request.not_before=f.events.now();gpu.enqueue(later);
    f.events.run();check(notices==3&&gpu.drained(),"idle GPU accepts later kernel traffic");
}
void sector_mask(){Fixture f;GpuControllerConfiguration c;c.sector_requests=true;c.issue_limit=256;c.issue_width=16;
    std::vector<GpuTraceRequest> trace;
    for(unsigned i=0;i<128;++i){
        auto r=request(i+1,Op::Write,i*32,32);
        for(unsigned b=0;b<32;++b)r.byte_mask.set((i%4)*32+b);
        trace.push_back(r);
    }
    auto duplicate=trace.front();duplicate.request.id=GlobalRequestId{129};trace.insert(trace.begin(),duplicate);
    GpuPageController gpu(f.host,f.events,std::move(trace),c);auto report=f.run(gpu);
    check(report.workload_success&&gpu.statistics().unique_write_bytes==4096,"32B mask windows cover exactly one page");
    check(gpu.statistics().page_writes==1&&gpu.statistics().completed==129,"duplicates do not inflate coverage");
}
}
int main(int argc,char** argv){
    const std::map<std::string,void(*)()> tests{{"merge",merge},{"write_coverage",write_coverage},{"partial",partial},{"feedback",feedback},{"multichannel",multichannel},{"recovery",recovery},{"issue_width",issue_width},{"streaming",streaming},{"sector_mask",sector_mask},{"external_clock",external_clock}};
    try{if(argc!=2||!tests.count(argv[1]))throw std::invalid_argument("unknown scenario");tests.at(argv[1])();std::cout<<"PASS gpu_pages_"<<argv[1]<<"\n";return 0;}
    catch(const std::exception& e){std::cerr<<"FAIL gpu pages: "<<e.what()<<"\n";return 1;}
}
