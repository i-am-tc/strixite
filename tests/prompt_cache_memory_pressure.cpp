#include "serve/prompt_cache.hpp"
#include "serve/log.hpp"
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace strix;
uint64_t rss() {std::ifstream f("/proc/self/statm");uint64_t all=0,resident=0;f>>all>>resident;return resident*(uint64_t)sysconf(_SC_PAGESIZE);}
void check(bool ok,const char*why) {if(!ok)throw std::runtime_error(why);}
int main(int argc,char**argv) {try {
 if(argc<2||argc>3)return 2;
 const bool blocked=argc==3;
 set_log_level(LogLevel::Error);
 std::filesystem::remove_all(argv[1]);
 const uint64_t MiB=1ull<<20,baseline=rss(),capacity=baseline+24*MiB,state_size=2*MiB;
 PromptCache::Options opt;opt.ram_margin=4*MiB;opt.prefault=false;opt.max_spares=0;opt.disk_free_margin=0;opt.idle_seconds=3600;opt.write_gib_per_hour=blocked?0.00001:0;opt.shutdown_seconds=0;
 opt.mem_available=[&]{const uint64_t used=rss();return used<capacity?capacity-used:0;};
 PromptCache cache(argv[1],16*MiB,"independent-pressure",opt);
 uint64_t peak=0;
 for(int i=0;i<48;++i){
  check(cache.make_room_for(state_size),"could not make room for independent history");
  HostBuffer b(state_size);std::memset(b.data(),i,b.size());
  std::vector<int32_t> ids(2048,i+1000);check(cache.put(std::move(ids),PromptCache::Kind::Turn,std::move(b)),"save refused");
  peak=std::max(peak,rss());
 }
 cache.flush();auto stats=cache.stats();std::vector<int32_t> oldest(2048,1000);
 check(stats.ram_evicted>0,"no RAM eviction for independent histories");
 if(blocked) {
  check(stats.rejected_budget>0,"write budget did not refuse writes");
  check(stats.writes_evict==0,"over-budget writes reached disk");
 } else {
  check(stats.writes_evict>0,"no disk spill for independent histories");
  check(stats.evicted>0,"no disk LRU eviction at the cap");
 }
 check(stats.bytes<=16*MiB,"disk cap exceeded");
 check(stats.ram_bytes<=24*MiB,"RAM entries grew beyond simulated free-memory budget");
 check(!cache.contains(oldest.data(),oldest.size()),"oldest history never expired");
 check(peak<baseline+40*MiB,"RSS grew linearly across independent histories");
 std::cout<<"PASS: 48 independent histories; offered 96 MiB, RSS growth peak "<<(peak-baseline)/(double)MiB<<" MiB; RAM "<<stats.ram_bytes/(double)MiB<<" MiB; RAM evictions "<<stats.ram_evicted<<"; disk writes "<<stats.writes_evict<<"; disk evictions "<<stats.evicted<<"; disk "<<stats.bytes/(double)MiB<<" MiB; oldest history removed\n";
 }catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
