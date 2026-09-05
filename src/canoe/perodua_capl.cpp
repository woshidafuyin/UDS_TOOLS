// CANoe owns the CAN channel. This DLL adapts CAPL frames to the same tested
// Perodua workflow used by Diagnostic Studio; it never opens a hardware driver.
#include "flash/perodua_p02c_workflow.hpp"
#include "core/profile.hpp"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstring>
#include <stdexcept>
#include "cdll.h"

namespace {
using namespace std::chrono_literals;
struct State {
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<uds::CanFrame> rx, tx;
  std::deque<std::string> logs;
  std::atomic<long> status{0}, percent{0};
  std::uint64_t submitted{}, acknowledged{};
  bool stopped{};
  std::jthread worker;
  std::wstring texts[5];
  unsigned long numbers[5]{0,0,0,0,0};
  void log(const std::string& s) {
    std::lock_guard lock(mutex);
    if (logs.size() >= 2048) logs.pop_front();
    logs.push_back(s);
  }
};
State state;
std::wstring wide(const char* s) {
  if (!s || !*s) return {};
  int n=MultiByteToWideChar(CP_ACP,0,s,-1,nullptr,0);
  std::wstring r(n,L'\0');
  MultiByteToWideChar(CP_ACP,0,s,-1,r.data(),n);
  r.pop_back(); return r;
}
std::filesystem::path project_root() {
  HMODULE module{};
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&project_root), &module);
  wchar_t path[32768]{};
  if (!GetModuleFileNameW(module,path,32768)) throw std::runtime_error("cannot locate Perodua DLL");
  return std::filesystem::path(path).parent_path().parent_path();
}
class CaplBus final : public uds::ICanBus {
public:
  void open() override {}
  void close() noexcept override {}
  bool is_open() const noexcept override { return true; }
  void send(const uds::CanFrame& frame) override {
    std::unique_lock lock(state.mutex);
    if(state.stopped) throw std::runtime_error("CANoe flashing stopped");
    const auto ticket=++state.submitted;
    state.tx.push_back(frame);
    if(!state.changed.wait_for(lock,150ms,[&]{return state.stopped || state.acknowledged>=ticket;}))
      throw std::runtime_error("CANoe CAPL transmit pump timeout");
    if(state.stopped) throw std::runtime_error("CANoe flashing stopped");
  }
  std::optional<uds::CanFrame> receive(std::chrono::milliseconds timeout) override {
    std::unique_lock lock(state.mutex);
    state.changed.wait_for(lock,timeout,[&]{return state.stopped || !state.rx.empty();});
    if(state.stopped) throw std::runtime_error("CANoe flashing stopped");
    if(state.rx.empty()) return {};
    auto f=std::move(state.rx.front());state.rx.pop_front();return f;
  }
};
class CaplProvider final : public uds::ICanBusProvider {
public:
  std::unique_ptr<uds::ICanBus> create(uds::CanChannelConfig) const override {
    return std::make_unique<CaplBus>();
  }
};
}

extern "C" {
__declspec(dllexport) void __cdecl p02Stop() {
  {std::lock_guard lock(state.mutex); state.stopped=true; state.tx.clear();}
  state.worker.request_stop();state.changed.notify_all();
  if(state.worker.joinable()) state.worker.join();
}
__declspec(dllexport) long __cdecl p02Text(unsigned long field,const char* value) {
  if(state.status==1 || field>=5) return 0;
  state.texts[field]=wide(value);return 1;
}
__declspec(dllexport) long __cdecl p02Number(unsigned long field,unsigned long value) {
  if(state.status==1 || field>=5) return 0;
  state.numbers[field]=value;return 1;
}
__declspec(dllexport) long __cdecl p02Begin() {
  if(state.status==1) return 0;
  p02Stop();
  {std::lock_guard lock(state.mutex);state.stopped=false;state.rx.clear();state.tx.clear();
    state.logs.clear();state.submitted=state.acknowledged=0;}
  state.percent=0;state.status=1;
  try {
    const auto root=project_root();
    uds::FlashJob job;
    job.profile=uds::load_profile_ini(root / "profiles/perodua_p02c.ini");
    job.can_bus_provider=std::make_shared<CaplProvider>();
    job.executable_directory=root;
    if(state.numbers[0]>2) throw std::runtime_error("select APP, CAL or APP+CAL");
    job.entry_mode=state.numbers[0]==0?L"app":state.numbers[0]==1?L"cal":L"app_cal";
    job.profile.programming_crc_variant=state.numbers[1]==1?L"reflected":
        state.numbers[1]==2?L"non_reflected":L"";
    job.profile.driver_start=state.numbers[2];job.profile.app_start=state.numbers[3];
    job.profile.cal_start=state.numbers[4];
    job.driver_file=state.texts[0];job.app_file=state.texts[1];job.cal_file=state.texts[2];
    job.security_key_file=state.texts[3];job.profile.programming_tester_identity=state.texts[4];
    state.worker=std::jthread([job=std::move(job)](std::stop_token stop){
      uds::FlashWorkflowCallbacks cb;
      cb.log=[](const std::string& s){state.log(s);};
      cb.progress=[](int p,const std::string&){state.percent=p;};
      try {uds::PeroduaP02cWorkflow().run(job,cb,stop);state.status=2;}
      catch(const std::exception& e){state.log(e.what());state.status=stop.stop_requested()?-2:-1;}
    });
    return 1;
  }catch(const std::exception& e){state.log(e.what());state.status=-1;return 0;}
}
__declspec(dllexport) long __cdecl p02Status(){return state.status;}
__declspec(dllexport) long __cdecl p02Progress(){return state.percent;}
__declspec(dllexport) unsigned long __cdecl p02Tx(unsigned long capacity,unsigned char* bytes){
  if(capacity<8 || !bytes) return 0;
  std::lock_guard lock(state.mutex);
  if(state.stopped || state.tx.empty()) return 0;
  auto f=std::move(state.tx.front());state.tx.pop_front();
  std::copy(f.data.begin(),f.data.end(),bytes);return f.id;
}
__declspec(dllexport) void __cdecl p02Ack(){
  {std::lock_guard lock(state.mutex);++state.acknowledged;}
  state.changed.notify_all();
}
__declspec(dllexport) void __cdecl p02Rx(unsigned long id,unsigned long size,const unsigned char* data){
  if(state.status!=1 || (id!=0x794 && id!=0x781) || size!=8 || !data) return;
  {std::lock_guard lock(state.mutex);
    if(state.rx.size()<4096) state.rx.push_back({id,{data,data+size},false,false,false});}
  state.changed.notify_all();
}
__declspec(dllexport) long __cdecl p02Log(unsigned long size,char* text){
  if(!text || size<2)return 0;
  std::lock_guard lock(state.mutex);
  if(state.logs.empty()){text[0]=0;return 0;}
  auto line=std::move(state.logs.front());state.logs.pop_front();
  const auto count=std::min<std::size_t>(size-1,line.size());
  std::memcpy(text,line.data(),count);text[count]=0;return 1;
}
}

CAPL_DLL_INFO4 table[]={
  {CDLL_VERSION_NAME,(CAPL_FARCALL)CDLL_VERSION,"","",CAPL_DLL_CDECL,0xabcd,CDLL_EXPORT},
  {"p02Begin",(CAPL_FARCALL)p02Begin,"Perodua","Start selected flash mode",'L',0,"","",{""}},
  {"p02Stop",(CAPL_FARCALL)p02Stop,"Perodua","Stop and join worker",'V',0,"","",{""}},
  {"p02Text",(CAPL_FARCALL)p02Text,"Perodua","Set file or tester identity",'L',2,"DC","\000\001",{"field","value"}},
  {"p02Number",(CAPL_FARCALL)p02Number,"Perodua","Set mode CRC or BIN address",'L',2,"DD","",{"field","value"}},
  {"p02Status",(CAPL_FARCALL)p02Status,"Perodua","1 running 2 pass -1 fail -2 stopped",'L',0,"","",{""}},
  {"p02Progress",(CAPL_FARCALL)p02Progress,"Perodua","Progress percent",'L',0,"","",{""}},
  {"p02Tx",(CAPL_FARCALL)p02Tx,"Perodua","Next Classic CAN frame",'D',2,"DB","\000\001",{"capacity","data"}},
  {"p02Ack",(CAPL_FARCALL)p02Ack,"Perodua","Frame submitted to CANoe",'V',0,"","",{""}},
  {"p02Rx",(CAPL_FARCALL)p02Rx,"Perodua","CANoe received frame",'V',3,"DDB","\000\000\001",{"id","size","data"}},
  {"p02Log",(CAPL_FARCALL)p02Log,"Perodua","Next workflow log",'L',2,"DC","\000\001",{"capacity","text"}},
  {0,0}
};
extern "C" __declspec(dllexport) CAPL_DLL_INFO4* caplDllTable4=table;
