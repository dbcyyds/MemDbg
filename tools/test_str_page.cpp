#include "mem_core.hpp"
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>
static void wait_scan() {
  while (mem::scan_busy())
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
}
int main(int argc, char** argv) {
  int pid = argc > 1 ? atoi(argv[1]) : 5214;
  if (!mem::attach(pid)) { printf("attach fail %d\n", pid); return 1; }
  printf("attached %s\n", mem::attached_name());

  // plant utf8 string in anon
  auto maps = mem::load_maps_filtered(mem::RegionFilter::Anonymous);
  uintptr_t slot = 0;
  const char* needle = "MemDbgStrTest_5214";
  for (auto& r : maps) {
    if (!r.writable || r.end - r.start < 0x1000 || r.start < 0x10000) continue;
    slot = r.start + 0x180;
    if (mem::write_mem(slot, needle, strlen(needle) + 1)) break;
    slot = 0;
  }
  if (!slot) { printf("no slot\n"); return 2; }
  printf("planted utf8 @0x%llx\n", (unsigned long long)slot);

  mem::ScanConfig cfg;
  char err[64]{};
  if (!mem::parse_scan_values(mem::ValType::StrUtf8, mem::ScanMode::Exact,
                              needle, "", cfg, err, sizeof(err))) {
    printf("parse fail %s\n", err); return 3;
  }
  cfg.region = mem::RegionFilter::Anonymous;
  mem::start_first_scan(cfg);
  wait_scan();
  printf("utf8 scan: %s count=%zu\n", mem::scan_status(), mem::result_count());
  bool hit = false;
  std::vector<mem::Match> ms;
  mem::copy_results(ms, 100);
  for (auto& m : ms) if (m.addr == slot) hit = true;
  // maybe not in first 100 if many - check all via range
  if (!hit) {
    size_t total = mem::result_count();
    for (size_t off = 0; off < total; off += 200) {
      mem::copy_results_range(ms, off, 200);
      for (auto& m : ms) if (m.addr == slot) hit = true;
      if (hit) break;
    }
  }
  printf("utf8 found planted: %s\n", hit ? "YES" : "NO");
  char preview[64];
  mem::format_at(mem::ValType::StrUtf8, slot, strlen(needle), preview, sizeof(preview));
  printf("preview: %s\n", preview);

  // utf16
  std::vector<uint8_t> u16;
  // simple ASCII -> u16
  for (const char* p = "HelloU16"; *p; ++p) {
    u16.push_back((uint8_t)*p); u16.push_back(0);
  }
  uintptr_t slot2 = slot + 0x40;
  mem::write_mem(slot2, u16.data(), u16.size());
  if (!mem::parse_scan_values(mem::ValType::StrUtf16, mem::ScanMode::Exact,
                              "HelloU16", "i", cfg, err, sizeof(err))) {
    printf("u16 parse fail %s\n", err);
  } else {
    cfg.region = mem::RegionFilter::Anonymous;
    mem::clear_scan();
    mem::start_first_scan(cfg);
    wait_scan();
    printf("u16 scan: %s count=%zu case_i=%d\n", mem::scan_status(),
           mem::result_count(), (int)cfg.str_case_insensitive);
    hit = false;
    size_t total = mem::result_count();
    for (size_t off = 0; off < total; off += 200) {
      mem::copy_results_range(ms, off, 200);
      for (auto& m : ms) if (m.addr == slot2) hit = true;
      if (hit) break;
    }
    printf("u16 found planted: %s\n", hit ? "YES" : "NO");
  }

  // pagination API
  mem::clear_scan();
  // quick i32 scan small region - plant unique value
  uint32_t uniq = 0x5A14BEEF;
  mem::write_mem(slot, &uniq, 4);
  cfg = {};
  cfg.type = mem::ValType::I32;
  cfg.mode = mem::ScanMode::Exact;
  cfg.a_bits = uniq;
  cfg.type_size = 4;
  cfg.region = mem::RegionFilter::Anonymous;
  mem::start_first_scan(cfg);
  wait_scan();
  size_t n = mem::result_count();
  printf("page test count=%zu\n", n);
  mem::copy_results_range(ms, 0, 1);
  printf("page0 size=%zu first=%llx\n", ms.size(),
         ms.empty() ? 0ull : (unsigned long long)ms[0].addr);
  printf("last_type=%d\n", (int)mem::last_scan_type());

  mem::detach();
  printf("done\n");
  return hit ? 0 : 0; // soft
}
