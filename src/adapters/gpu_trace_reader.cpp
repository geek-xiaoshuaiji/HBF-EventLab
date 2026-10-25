#include "hbf_eventlab/adapters/gpu_trace_reader.hpp"
#include <set>
#include <sstream>

namespace hbf_eventlab {
namespace {
std::uint64_t number(const std::string& s) {
    if(s.empty()||s.front()=='-'||s.front()=='+')throw std::invalid_argument("expected unsigned integer");
    std::size_t n=0;auto value=std::stoull(s,&n,s.rfind("0x",0)==0||s.rfind("0X",0)==0?16:10);
    if(n!=s.size())throw std::invalid_argument("invalid integer suffix");return value;
}
}
std::vector<GpuTraceRequest> read_gpu_trace(std::istream& in,Tick cycle_time) {
    if(!cycle_time.value)throw std::invalid_argument("GPU cycle time must be positive");
    std::vector<GpuTraceRequest> out;std::string line;std::size_t lineno=0;bool header=false;
    while(std::getline(in,line)) {
        ++lineno;line=line.substr(0,line.find('#'));std::istringstream words(line);std::string op;
        if(!(words>>op))continue;
        try {
            if(!header) {
                std::string version,extra;
                if(op!="HBF_EVENTLAB_GPU_TRACE"||!(words>>version)||version!="1"||(words>>extra))
                    throw std::invalid_argument("expected HBF_EVENTLAB_GPU_TRACE 1 header");
                header=true;continue;
            }
            std::string id,cycle,address,bytes;
            if(!(words>>id>>cycle>>address>>bytes)||(op!="READ"&&op!="WRITE"))
                throw std::invalid_argument("expected READ/WRITE id gpu_cycle global_byte bytes");
            GpuTraceRequest item;
            item.gpu_cycle=number(cycle);
            item.request={GlobalRequestId{number(id)},op=="READ"?Op::Read:Op::Write,
                GlobalByteAddress{number(address)},number(bytes)};
            item.request.not_before=Tick{checked_mul(item.gpu_cycle,cycle_time.value)};
            std::set<std::string> fields;std::string field;
            while(words>>field) {
                const auto equal=field.find('=');if(equal==std::string::npos)throw std::invalid_argument("expected key=value");
                const auto key=field.substr(0,equal),value=field.substr(equal+1);
                if(!fields.insert(key).second)throw std::invalid_argument("duplicate field");
                if(key=="content")item.request.content=number(value);
                else if(key=="expect")item.request.expected_content=number(value);
                else if(key=="source") {const auto n=number(value);if(n>UINT32_MAX)throw std::invalid_argument("source overflow");item.source=static_cast<std::uint32_t>(n);}
                else if(key=="after") {
                    if(value=="none")continue;
                    if(value.empty()||value.back()==',')throw std::invalid_argument("empty dependency");
                    std::istringstream deps(value);std::string dependency;
                    while(std::getline(deps,dependency,','))item.request.after.push_back(GlobalRequestId{number(dependency)});
                } else throw std::invalid_argument("unknown GPU trace field");
            }
            if(item.request.op==Op::Write&&!fields.count("content"))throw std::invalid_argument("WRITE requires content identity");
            if(item.request.op==Op::Read&&fields.count("content"))throw std::invalid_argument("READ uses expect, not content");
            out.push_back(std::move(item));
        } catch(const std::exception& e) {throw std::invalid_argument("GPU trace line "+std::to_string(lineno)+": "+e.what());}
    }
    if(!header)throw std::invalid_argument("missing GPU trace version header");
    return out;
}
} // namespace hbf_eventlab
