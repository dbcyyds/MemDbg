#include "mem_core.hpp"
#include "mem_lua.hpp"
#include "mem_game.hpp"
#include "mem_disasm.hpp"
#include <cstdio>
int main(int c,char**v){
  int pid=c>1?atoi(v[1]):0;
  if(!mem::attach(pid)){printf("attach fail\n");return 1;}
  mem::sym_refresh();
  char e[256]={};
  const char* scr=
    "print('pid', mem.pid(), mem.name())\n"
    "local s,en = mem.module_base('libc.so')\n"
    "print('libc', s, en)\n"
    "local a,_ = mem.sym_find('getpid')\n"
    "print('getpid', a)\n"
    "local n = 0\n"
    "for i,m in ipairs(mem.modules('libdl')) do n=n+1 end\n"
    "print('libdl mods', n)\n"
    "print(mem.speed(2.0))\n"
    "print(mem.speed('off'))\n"
    "local ok,cnt = mem.scan_first('123456789', 'i32', 'exact', 'anon')\n"
    "print('scan', ok, mem.scan_wait(30000))\n"
    "mem.scan_clear()\n";
  bool ok=mem::script_run(scr,e,sizeof(e));
  printf("ok=%d err=%s\n---log---\n%s\n", (int)ok, e, mem::script_log());
  mem::script_shutdown();
  mem::detach();
  return ok?0:1;
}
