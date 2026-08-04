#include "mem_core.hpp"
#include "mem_disasm.hpp"
#include <cstdio>
#include <cstring>
int main(int argc, char** argv) {
  int pid = argc>1?atoi(argv[1]):29267;
  if (!mem::attach(pid)) { printf("attach fail\n"); return 1; }
  auto maps = mem::load_maps_filtered(mem::RegionFilter::Code);
  int shown=0;
  for (auto& r: maps) {
    if (r.path.find(".so")==std::string::npos) continue;
    if (r.end-r.start < 0x1000) continue;
    uintptr_t offs[] = {0, 0x1000, 0x10000, (r.end-r.start)/4};
    for (uintptr_t off : offs) {
      uintptr_t a = r.start + off;
      if (a+64 >= r.end) continue;
      mem::DisasmOptions o; o.filter_noise=false;
      auto ins = mem::disasm_at(a, 12, o);
      int real=0,noise=0,word=0;
      for (auto& i: ins) {
        if (!strcmp(i.mnem,".word")) word++;
        else if (i.is_noise) noise++;
        else real++;
      }
      if (real>=4) {
        printf("HIT %s +0x%llx real=%d noise=%d word=%d\n", r.path.c_str(), (unsigned long long)off, real, noise, word);
        for (int k=0;k<(int)ins.size()&&k<6;k++)
          printf("  %s %s\n", ins[k].mnem, ins[k].ops);
        shown++;
        break;
      }
    }
    if (shown>=3) break;
  }
  for (auto& m: maps) {
    if (m.path.find("libc.so")==std::string::npos) continue;
    mem::DisasmOptions o1; o1.filter_noise=false;
    mem::DisasmOptions o2; o2.filter_noise=true;
    uintptr_t offs[] = {0, 0x1000, 0x20000};
    for (uintptr_t off : offs) {
      auto a1=mem::disasm_at(m.start+off,32,o1);
      auto a2=mem::disasm_at(m.start+off,32,o2);
      int real=0; for(auto&i:a1) if(!i.is_noise && strcmp(i.mnem,".word")) real++;
      printf("libc +0x%llx filter off=%zu on=%zu real=%d\n", (unsigned long long)off, a1.size(), a2.size(), real);
      for (int k=0;k<(int)a1.size()&&k<6;k++)
        printf("  %s noise=%d\n", a1[k].mnem, (int)a1[k].is_noise);
    }
    break;
  }
  mem::detach();
  return 0;
}
