#include "mem_core.hpp"
#include "mem_ptrscan.hpp"
#include "mem_struct.hpp"
#include <cstdio>
#include <cstring>
#include <vector>
int main() {
  int pid = 0;
  // pick wechat or system_server
  FILE* p = popen("pidof com.tencent.mm 2>/dev/null; pidof system_server 2>/dev/null", "r");
  if (p) { fscanf(p, "%d", &pid); pclose(p); }
  if (pid <= 0) { printf("no pid\n"); return 1; }
  if (!mem::attach(pid)) { printf("attach fail\n"); return 1; }
  printf("pid=%d %s\n", pid, mem::attached_name());

  // plant pointer chain: slot2 -> slot1+0x10, slot1 holds target-0x20 so offset 0x20
  auto maps = mem::load_maps_filtered(mem::RegionFilter::Anonymous);
  uintptr_t area = 0;
  for (auto& r : maps) {
    if (r.writable && r.end - r.start > 0x2000 && r.start > 0x10000) {
      area = r.start + 0x300; break;
    }
  }
  if (!area) { printf("no area\n"); return 2; }
  uintptr_t target = area + 0x100;
  uintptr_t slot1 = area + 0x40;
  uintptr_t slot2 = area + 0x80;
  uint64_t v1 = target - 0x20;
  uint64_t v2 = slot1; // offset 0 from slot2 value to slot1... wait chain: read slot2 -> p, p+off0 = next, read next -> q, q+off1 = target
  // level1: someone points to target with offset: *slot1 = target - 0x20, offset=0x20
  // level2: *slot2 = slot1 - 0x0, offset=0; offsets [0, 0x20]
  mem::write_mem(slot1, &v1, 8);
  mem::write_mem(slot2, &v2, 8);
  printf("target=%llx slot1=%llx slot2=%llx\n", (unsigned long long)target,
         (unsigned long long)slot1, (unsigned long long)slot2);

  mem::PtrChain c;
  c.base = slot2;
  c.module = "anon";
  c.offsets = {0, 0x20};
  uintptr_t r = 0;
  bool ok = mem::ptrscan_resolve(c, r);
  printf("resolve path: ok=%d r=%llx expect=%llx %s\n", (int)ok,
         (unsigned long long)r, (unsigned long long)target,
         r == target ? "OK" : "FAIL");
  std::vector<uintptr_t> path;
  mem::ptrscan_resolve_path(c, path);
  printf("path steps=%zu\n", path.size());
  char fmt[128];
  mem::ptrscan_format(c, fmt, sizeof(fmt));
  printf("format: %s\n", fmt);

  // save load
  std::vector<mem::PtrChain> cs = {c};
  // absolute form
  c.module.clear();
  cs[0] = c;
  mem::ptrscan_save("/data/local/tmp/t_ptrs.txt", cs);
  std::vector<mem::PtrChain> loaded;
  int n = mem::ptrscan_load("/data/local/tmp/t_ptrs.txt", loaded);
  printf("save/load n=%d\n", n);

  // structure
  mem::Structure s;
  s.base = area;
  uint32_t a = 42, b = 100;
  float f = 3.14f;
  uint64_t pv = target;
  mem::write_mem(area, &a, 4);
  mem::write_mem(area + 4, &b, 4);
  mem::write_mem(area + 8, &f, 4);
  mem::write_mem(area + 16, &pv, 8);
  s.auto_dissect(8);
  printf("struct fields=%zu size~%d\n", s.fields.size(), s.total_size());
  for (size_t i = 0; i < s.fields.size() && i < 6; ++i)
    printf("  +0x%X %s %s = %s\n", s.fields[i].offset,
           mem::field_type_name(s.fields[i].type), s.fields[i].name,
           s.fields[i].value);
  s.save("/data/local/tmp/t_struct.txt");
  mem::Structure s2;
  printf("struct load=%d\n", s2.load("/data/local/tmp/t_struct.txt"));

  mem::detach();
  printf("done\n");
  return 0;
}
