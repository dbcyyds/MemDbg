#include "mem_core.hpp"
#include "mem_bp.hpp"
#include <cstdio>
#include <unistd.h>
int main(int c,char**v){
  int pid=atoi(v[1]);
  if(!mem::attach(pid)){printf("attach fail\n");return 1;}
  auto maps=mem::load_maps(true);
  uintptr_t a=0; uint32_t o=0;
  for(auto&r:maps){
    if(r.writable&&r.end-r.start>0x1000&&r.start>0x10000){
      if(mem::read_mem(r.start+0x300,&o,4)){a=r.start+0x300;break;}
    }
  }
  printf("addr=%llx\n",(unsigned long long)a);
  int id=mem::bp_set(a, mem::BpType::WatchW, 4);
  printf("set id=%d st=%s backend=%s\n", id, mem::bp_status(), mem::bp_backend_name());
  auto list=mem::bp_list();
  printf("list=%zu\n", list.size());
  for(auto&b:list) printf("  #%d type=%d addr=%llx note=%s\n", b.id,(int)b.type,(unsigned long long)b.addr,b.note);
  // try to trigger: many kernels don't fire on process_vm_writev from tracer
  // use remote_call? or just confirm set works
  // pause, read regs, resume
  mem::bp_arm_and_continue();
  // soft exec as comparison
  auto code=mem::load_maps_filtered(mem::RegionFilter::Code);
  uintptr_t ca=0;
  for(auto&r:code){ if(r.path.find("libc.so")!=std::string::npos){ca=(r.start+0x2000)&~3ull;break;}}
  if(!ca && !code.empty()) ca=(code[0].start+0x1000)&~3ull;
  int xid=mem::bp_set(ca, mem::BpType::Exec, 4);
  printf("exec id=%d st=%s @%llx\n", xid, mem::bp_status(), (unsigned long long)ca);
  list=mem::bp_list();
  printf("list after exec=%zu\n", list.size());
  mem::bp_clear_all();
  printf("cleared st=%s count=%d\n", mem::bp_status(), mem::bp_count());
  mem::bp_shutdown();
  mem::detach();
  printf("done\n");
  return id>=0?0:1;
}
