#include "mem_core.hpp"
#include "mem_bp.hpp"
#include <cstdio>
#include <cstring>
#include <string>
int main() {
  printf("fuzzy tencent/qq: %d\n", mem::fuzzy_match("com.tencent.mobileqq", "tx qq"));
  printf("fuzzy mobile: %d\n", mem::fuzzy_match("com.tencent.mobileqq", "mobile"));
  printf("fuzzy no: %d\n", mem::fuzzy_match("com.tencent.mobileqq", "wechat"));

  mem::ProcListOptions o;
  o.filter = "qq";
  o.skip_system = false;
  o.fuzzy_filter = true;
  o.skip_no_icon = false;
  auto p = mem::list_processes(o);
  printf("filter qq count=%zu\n", p.size());
  for (auto& x : p)
    if (x.pid == 5214 || x.name.find("qq") != std::string::npos)
      printf("  %d %s tencent=%d\n", x.pid, x.name.c_str(), (int)x.is_tencent);

  if (!mem::attach(5214)) { printf("attach fail\n"); return 1; }
  printf("attached %s\n", mem::attached_name());

  mem::ScanConfig cfg;
  char err[64]{};
  bool ok = mem::parse_scan_values(mem::ValType::Hex, mem::ScanMode::Exact,
                                   "?? 00 ?? FF", nullptr, cfg, err, sizeof(err));
  printf("hex wild parse=%d pat=%zu mask=%zu\n", (int)ok, cfg.hex_pat.size(),
         cfg.hex_mask.size());

  ok = mem::parse_scan_values(mem::ValType::I32, mem::ScanMode::Fuzzy, "100",
                              "5", cfg, err, sizeof(err));
  printf("fuzzy parse=%d tol=%g\n", (int)ok, cfg.fuzzy_tol);

  auto thr = mem::list_threads();
  printf("threads=%zu\n", thr.size());
  auto mods = mem::list_modules("libturing");
  printf("modules libturing=%zu\n", mods.size());
  if (!mods.empty())
    printf("  %s @%llx\n", mods[0].path.c_str(),
           (unsigned long long)mods[0].start);

  if (!mods.empty()) {
    uintptr_t a = mods[0].start;
    uint32_t before = 0;
    mem::read_mem(a, &before, 4);
    int id = mem::soft_bp_set(a);
    uint32_t mid = 0;
    mem::read_mem(a, &mid, 4);
    mem::soft_bp_clear(id);
    uint32_t after = 0;
    mem::read_mem(a, &after, 4);
    printf("soft_bp id=%d before=%08x mid=%08x after=%08x %s\n", id, before,
           mid, after,
           (before == after && mid == 0xD4200000u) ? "OK" : "CHECK");
  }

  mem::detach();
  printf("done\n");
  return 0;
}
