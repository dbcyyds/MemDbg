#include "mem_core.hpp"
#include "mem_bp.hpp"
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <errno.h>
int main(int c,char**v){
  int pid=c>1?atoi(v[1]):0;
  if(!mem::attach(pid)){printf("attach fail\n");return 1;}
  printf("attached %s\n", mem::attached_name());
  printf("bp_init=%d %s backend=%s\n", (int)mem::bp_init(), mem::bp_status(), mem::bp_backend_name());
  auto maps=mem::load_maps(true);
  uintptr_t a=0; uint32_t o=0;
  for(auto&r:maps){
    if(!r.writable||r.end-r.start<0x1000||r.start<0x10000) continue;
    if(mem::read_mem(r.start+0x200,&o,4)){a=r.start+0x200;break;}
  }
  printf("test_addr=%llx\n",(unsigned long long)a);
  int id=mem::bp_set(a, mem::BpType::WatchW, 4);
  printf("WatchW id=%d st=%s backend=%s\n", id, mem::bp_status(), mem::bp_backend_name());
  if(id>=0){
    mem::bp_arm_and_continue();
    uint32_t v=0xDEADBEEF;
    mem::write_mem(a,&v,4);
    usleep(200000);
    // also write from target? we can only write externally
    for(int i=0;i<30;i++){
      mem::BpHit h{};
      if(mem::bp_poll(h)&&h.valid){printf("HIT: %s\n", h.msg);break;}
      usleep(20000);
    }
    // try target self-write via remote? skip
    mem::bp_clear(id);
  }
  // exec bp
  auto code=mem::load_maps_filtered(mem::RegionFilter::Code);
  uintptr_t ca=0;
  for(auto&r:code){
    if(r.path.find(".so")==std::string::npos) continue;
    if(r.end-r.start<0x2000) continue;
    ca=(r.start+0x1000)&~3ull; break;
  }
  printf("code=%llx\n",(unsigned long long)ca);
  int xid=mem::bp_set(ca, mem::BpType::Exec, 4);
  printf("Exec id=%d st=%s backend=%s\n", xid, mem::bp_status(), mem::bp_backend_name());
  if(xid>=0) mem::bp_clear(xid);
  // ptrace path force
  printf("ptrace_active=%d\n", (int)mem::bp_ptrace_active());
  mem::bp_shutdown();
  mem::detach();
  return 0;
}
