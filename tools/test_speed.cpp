#include "mem_core.hpp"
#include "mem_game.hpp"
#include "mem_disasm.hpp"
#include "mem_bp.hpp"
#include <cstdio>
int main(int c,char**v){
  int pid=c>1?atoi(v[1]):0;
  if(!mem::attach(pid)){printf("attach fail\n");return 1;}
  printf("attached %s\n", mem::attached_name());
  int n=mem::sym_refresh();
  printf("syms=%d\n", n);
  bool ok=mem::speed_set(2.0f);
  printf("speed_set=%d st=%s active=%d mult=%g\n", (int)ok, mem::speed_status(),
         (int)mem::speed_active(), mem::speed_get());
  // script write examples
  char e[128];
  const char* scr=
    "help\n"
    "print test write\n"
    "write 0x2000100 i32 42\n"
    "read 0x2000100 i32\n"
    "writestr 0x2000200 \"HiMDS\"\n"
    "writehex 0x2000300 DE AD BE EF\n"
    "repeat 2\n"
    "  print loop\n"
    "endrep\n"
    "speed 1.5\n"
    "speed off\n";
  // plant writable
  auto maps=mem::load_maps(true);
  uintptr_t a=0;
  for(auto&r:maps){if(r.writable&&r.end-r.start>0x1000&&r.start>0x10000){a=r.start+0x100;break;}}
  char s2[512];
  snprintf(s2,sizeof(s2),
    "help\n"
    "write 0x%llx i32 42\n"
    "read 0x%llx i32\n"
    "writestr 0x%llx \"HiMDS\"\n"
    "writef 0x%llx 3.14\n"
    "repeat 2\n print loop\n endrep\n",
    (unsigned long long)a,(unsigned long long)a,
    (unsigned long long)(a+0x40),(unsigned long long)(a+0x80));
  ok=mem::script_run(s2,e,sizeof(e));
  printf("script ok=%d err=%s\n", (int)ok, e);
  printf("log:\n%s\n", mem::script_log());
  mem::speed_disable();
  printf("after disable: %s\n", mem::speed_status());
  mem::detach();
  return 0;
}
