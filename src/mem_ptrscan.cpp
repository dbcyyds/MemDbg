#include "mem_ptrscan.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mem {
namespace {

std::mutex g_mu;
std::atomic<bool> g_busy{false};
std::atomic<float> g_prog{0.f};
std::atomic<bool> g_cancel{false};
char g_status[128] = "指针扫描空闲";
std::thread g_th;
std::vector<PtrChain> g_results;

void set_st(const char* s) {
  std::snprintf(g_status, sizeof(g_status), "%s", s ? s : "");
}

bool is_static_region(const Region& r) {
  if (r.path.empty()) return false;
  if (r.path[0] == '[') return false;  // [anon:...] [heap]
  // 文件映射
  return true;
}

// 在可读内存中查找「指向 [target-maxOff, target] 的指针」
struct PtrHit {
  uintptr_t at = 0;      // 存放指针的地址
  int32_t offset = 0;    // target - *at
  uintptr_t value = 0;   // 读到的指针值
};

[[maybe_unused]] void scan_level(const std::vector<Region>& regs,
                                  uintptr_t target_lo, uintptr_t target_hi,
                                  std::vector<PtrHit>& out, size_t max_hits) {
  constexpr size_t kChunk = 256 * 1024;
  std::vector<uint8_t> buf(kChunk + 8);
  size_t total = 0;
  for (auto& r : regs) total += r.end - r.start;
  if (total == 0) total = 1;
  size_t done = 0;

  for (auto& r : regs) {
    if (g_cancel.load()) break;
    uintptr_t addr = (r.start + 7) & ~7ull;  // 8 对齐
    while (addr + 8 <= r.end && !g_cancel.load()) {
      size_t want = std::min((size_t)(r.end - addr), kChunk);
      want &= ~7ull;
      if (want < 8) break;
      if (!read_mem(addr, buf.data(), want)) {
        addr += want ? want : 0x1000;
        done += want;
        continue;
      }
      for (size_t i = 0; i + 8 <= want; i += 8) {
        uint64_t v = 0;
        std::memcpy(&v, buf.data() + i, 8);
        if (v >= target_lo && v <= target_hi) {
          PtrHit h;
          h.at = addr + i;
          h.value = (uintptr_t)v;
          h.offset = (int32_t)((int64_t)target_hi - (int64_t)v);
          // 实际 offset = target - ptr_value，target 用 hi 不准确
          // 调用方会用真实 target
          out.push_back(h);
          if (out.size() >= max_hits) return;
        }
      }
      addr += want;
      done += want;
      g_prog.store(std::min(0.99f, (float)done / (float)total));
    }
  }
}

void worker(PtrScanConfig cfg) {
  set_st("指针扫描中…");
  g_prog = 0.f;
  auto all = load_maps(false);
  std::vector<Region> scan_regs;
  std::vector<Region> static_regs;
  for (auto& r : all) {
    if (!r.readable) continue;
    if (r.writable) scan_regs.push_back(r);
    if (is_static_region(r) && r.readable) static_regs.push_back(r);
  }
  if (scan_regs.empty()) scan_regs = all;

  // level 1: 谁指向 target 附近
  const uintptr_t tgt = cfg.target;
  const uintptr_t lo = tgt > cfg.max_offset ? tgt - cfg.max_offset : 0;
  const uintptr_t hi = tgt;

  std::vector<PtrHit> level1;
  level1.reserve(4096);
  // 精确：指针值在 [tgt-maxOff, tgt]，offset = tgt - value
  {
    constexpr size_t kChunk = 256 * 1024;
    std::vector<uint8_t> buf(kChunk + 8);
    size_t total = 0;
    for (auto& r : scan_regs) total += r.end - r.start;
    if (!total) total = 1;
    size_t done = 0;
    for (auto& r : scan_regs) {
      if (g_cancel.load()) break;
      uintptr_t addr = (r.start + 7) & ~7ull;
      while (addr + 8 <= r.end && !g_cancel.load()) {
        size_t want = std::min((size_t)(r.end - addr), kChunk) & ~7ull;
        if (want < 8) break;
        if (!read_mem(addr, buf.data(), want)) {
          addr += 0x1000;
          done += 0x1000;
          continue;
        }
        for (size_t i = 0; i + 8 <= want; i += 8) {
          uint64_t v = 0;
          std::memcpy(&v, buf.data() + i, 8);
          if (v >= lo && v <= hi) {
            PtrHit h;
            h.at = addr + i;
            h.value = (uintptr_t)v;
            h.offset = (int32_t)((int64_t)tgt - (int64_t)v);
            level1.push_back(h);
            if (level1.size() >= 50000) goto done_l1;
          }
        }
        addr += want;
        done += want;
        g_prog.store(0.3f * (float)done / (float)total);
      }
    }
  }
done_l1:

  std::vector<PtrChain> results;
  auto fill_module = [&](PtrChain& c, uintptr_t a) {
    for (auto& r : static_regs) {
      if (a >= r.start && a < r.end) {
        c.module_base = r.start;
        c.base_rva = a - r.start;
        c.module_path = r.path;
        auto slash = r.path.find_last_of('/');
        c.module =
            slash == std::string::npos ? r.path : r.path.substr(slash + 1);
        return;
      }
    }
    // 也查全部映射
    for (auto& r : all) {
      if (a >= r.start && a < r.end && !r.path.empty() && r.path[0] != '[') {
        c.module_base = r.start;
        c.base_rva = a - r.start;
        c.module_path = r.path;
        auto slash = r.path.find_last_of('/');
        c.module =
            slash == std::string::npos ? r.path : r.path.substr(slash + 1);
        return;
      }
    }
    c.module_base = 0;
    c.base_rva = 0;
    c.module = "anon";
  };
  auto in_static = [&](uintptr_t a) -> bool {
    for (auto& r : static_regs)
      if (a >= r.start && a < r.end) return true;
    return false;
  };

  // 1 级：静态区命中直接成链
  for (auto& h : level1) {
    if (cfg.static_only && !in_static(h.at)) continue;
    PtrChain c;
    c.base = h.at;
    fill_module(c, h.at);
    if (c.module == "anon" && cfg.static_only) continue;
    c.offsets.push_back(h.offset);
    c.resolved = tgt;
    c.valid = true;
    results.push_back(c);
    if ((int)results.size() >= cfg.max_results) break;
  }

  // 多级：对非静态的 level1 指针地址，再找谁指向它们
  if (cfg.max_level >= 2 && (int)results.size() < cfg.max_results) {
    // 取前 N 个非静态指针位置作为下一目标
    std::vector<PtrHit> seeds;
    for (auto& h : level1) {
      if (in_static(h.at)) continue;
      seeds.push_back(h);
      if (seeds.size() >= 800) break;
    }

    size_t si = 0;
    for (auto& seed : seeds) {
      if (g_cancel.load()) break;
      if ((int)results.size() >= cfg.max_results) break;
      uintptr_t mid = seed.at;
      uintptr_t mlo = mid > cfg.max_offset ? mid - cfg.max_offset : 0;
      std::vector<PtrHit> l2;
      // 轻量扫：只扫 static + 部分 rw
      std::vector<Region> regs2 = static_regs;
      for (auto& r : scan_regs) {
        if (regs2.size() > 200) break;
        if (!is_static_region(r)) regs2.push_back(r);
      }
      for (auto& r : regs2) {
        if (!r.readable) continue;
        uintptr_t addr = (r.start + 7) & ~7ull;
        uint8_t page[4096];
        while (addr + 8 <= r.end) {
          size_t want = std::min((size_t)(r.end - addr), sizeof(page)) & ~7ull;
          if (want < 8) break;
          if (!read_mem(addr, page, want)) {
            addr += 0x1000;
            continue;
          }
          for (size_t i = 0; i + 8 <= want; i += 8) {
            uint64_t v = 0;
            std::memcpy(&v, page + i, 8);
            if (v >= mlo && v <= mid) {
              PtrHit h2;
              h2.at = addr + i;
              h2.value = (uintptr_t)v;
              h2.offset = (int32_t)((int64_t)mid - (int64_t)v);
              // 只要静态基址
              if (cfg.static_only && !in_static(h2.at)) continue;
              PtrChain c;
              c.base = h2.at;
              fill_module(c, h2.at);
              if (c.module == "anon" && cfg.static_only) continue;
              c.offsets.push_back(h2.offset);
              c.offsets.push_back(seed.offset);
              c.resolved = tgt;
              c.valid = true;
              results.push_back(c);
              if ((int)results.size() >= cfg.max_results) goto finish;
            }
          }
          addr += want;
        }
      }
      si++;
      g_prog.store(0.3f + 0.7f * (float)si / (float)std::max<size_t>(seeds.size(), 1));
    }
  }

  // 3 级简化：在 2 级结果上不再深扫（性能）；用户可提高 level 时做有限扩展
  if (cfg.max_level >= 3 && (int)results.size() < cfg.max_results / 2) {
    // 对部分 2 级链的 base 再扫一层静态（代价高，限制 50 条种子）
    std::vector<PtrChain> extra;
    int seeds = 0;
    auto copy = results;
    for (auto& ch : copy) {
      if (ch.offsets.size() != 2) continue;
      if (seeds++ > 40) break;
      uintptr_t mid = ch.base;
      uintptr_t mlo = mid > cfg.max_offset ? mid - cfg.max_offset : 0;
      for (auto& r : static_regs) {
        uintptr_t addr = (r.start + 7) & ~7ull;
        uint8_t page[4096];
        int pages = 0;
        while (addr + 8 <= r.end && pages < 64) {
          size_t want = std::min((size_t)(r.end - addr), sizeof(page)) & ~7ull;
          if (want < 8) break;
          if (!read_mem(addr, page, want)) {
            addr += 0x1000;
            pages++;
            continue;
          }
          for (size_t i = 0; i + 8 <= want; i += 8) {
            uint64_t v = 0;
            std::memcpy(&v, page + i, 8);
            if (v >= mlo && v <= mid) {
              PtrChain c;
              c.base = addr + i;
              fill_module(c, c.base);
              if (c.module == "anon" || c.module.empty()) continue;
              c.offsets.push_back((int32_t)((int64_t)mid - (int64_t)v));
              c.offsets.insert(c.offsets.end(), ch.offsets.begin(),
                               ch.offsets.end());
              c.resolved = tgt;
              c.valid = true;
              extra.push_back(c);
              if ((int)results.size() + (int)extra.size() >= cfg.max_results)
                goto merge3;
            }
          }
          addr += want;
          pages++;
        }
      }
    }
  merge3:
    results.insert(results.end(), extra.begin(), extra.end());
  }

finish:
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_results.swap(results);
  }
  char buf[96];
  std::snprintf(buf, sizeof(buf), "指针扫描完成 · %zu 条", g_results.size());
  set_st(buf);
  g_prog = 1.f;
  g_busy = false;
}

}  // namespace

bool ptrscan_start(const PtrScanConfig& cfg) {
  if (!is_attached()) {
    set_st("请先附加进程");
    return false;
  }
  if (cfg.target == 0) {
    set_st("目标地址无效");
    return false;
  }
  if (g_busy.load()) return false;
  if (g_th.joinable()) g_th.join();
  g_cancel = false;
  g_busy = true;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_results.clear();
  }
  PtrScanConfig c = cfg;
  if (c.max_level < 1) c.max_level = 1;
  if (c.max_level > 5) c.max_level = 5;
  if (c.max_offset < 0x10) c.max_offset = 0x10;
  if (c.max_offset > 0x10000) c.max_offset = 0x10000;
  g_th = std::thread([c]() { worker(c); });
  return true;
}

bool ptrscan_busy() { return g_busy.load(); }
float ptrscan_progress() { return g_prog.load(); }
const char* ptrscan_status() { return g_status; }

void ptrscan_clear() {
  g_cancel = true;
  if (g_th.joinable()) g_th.join();
  g_busy = false;
  std::lock_guard<std::mutex> lk(g_mu);
  g_results.clear();
  set_st("已清空指针结果");
}

size_t ptrscan_count() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_results.size();
}

void ptrscan_copy(std::vector<PtrChain>& out, size_t max_n) {
  std::lock_guard<std::mutex> lk(g_mu);
  size_t n = std::min(max_n, g_results.size());
  out.assign(g_results.begin(), g_results.begin() + (std::ptrdiff_t)n);
}

bool ptrscan_resolve(const PtrChain& chain, uintptr_t& out_addr) {
  std::vector<uintptr_t> path;
  if (!ptrscan_resolve_path(chain, path) || path.empty()) return false;
  out_addr = path.back();
  return true;
}

bool ptrscan_resolve_path(const PtrChain& chain, std::vector<uintptr_t>& path) {
  path.clear();
  if (chain.offsets.empty()) return false;
  uintptr_t cur = chain.base;
  path.push_back(cur);  // 指针槽
  for (size_t i = 0; i < chain.offsets.size(); ++i) {
    uint64_t ptr = 0;
    if (!read_mem(cur, &ptr, 8)) return false;
    cur = (uintptr_t)ptr + (int64_t)chain.offsets[i];
    path.push_back(cur);
  }
  return true;
}

void ptrscan_format(const PtrChain& chain, char* buf, size_t cap) {
  if (!buf || cap < 8) return;
  int n = 0;
  if (!chain.module.empty() && chain.module != "anon" &&
      (chain.module_base || chain.base_rva)) {
    n = std::snprintf(buf, cap, "%s+0x%llX", chain.module.c_str(),
                      (unsigned long long)chain.base_rva);
  } else {
    n = std::snprintf(buf, cap, "0x%llX", (unsigned long long)chain.base);
  }
  for (int32_t off : chain.offsets) {
    if (n >= (int)cap - 12) break;
    n += std::snprintf(buf + n, cap - (size_t)n, " -> +0x%X", (unsigned)off);
  }
}

bool find_module_base(const char* module_name, uintptr_t& out_base,
                      uintptr_t& out_end, std::string* full_path) {
  out_base = out_end = 0;
  if (!module_name || !module_name[0] || !is_attached()) return false;
  auto maps = load_maps(false);
  for (auto& r : maps) {
    if (r.path.empty()) continue;
    auto slash = r.path.find_last_of('/');
    std::string base =
        slash == std::string::npos ? r.path : r.path.substr(slash + 1);
    if (base == module_name || r.path.find(module_name) != std::string::npos) {
      if (out_base == 0 || r.start < out_base) {
        out_base = r.start;
        if (full_path) *full_path = r.path;
      }
      if (r.end > out_end) out_end = r.end;
    }
  }
  return out_base != 0;
}

bool ptrscan_rebind(PtrChain& chain) {
  if (chain.module.empty() || chain.module == "anon") return false;
  uintptr_t mb = 0, me = 0;
  std::string path;
  if (!find_module_base(chain.module.c_str(), mb, me, &path)) return false;
  chain.module_base = mb;
  chain.module_path = path;
  chain.base = mb + chain.base_rva;
  return true;
}

bool ptrscan_parse_template(const char* text, PtrChain& out) {
  // 格式: name+0xRVA,off0,off1  或  0xBASE,off0,off1
  out = {};
  if (!text || !text[0]) return false;
  char tmp[256];
  std::snprintf(tmp, sizeof(tmp), "%s", text);
  // 去掉空格与箭头
  std::string s;
  for (char* p = tmp; *p; ++p) {
    if (*p == ' ' || *p == '\t') continue;
    if (p[0] == '-' && p[1] == '>') {
      p++;
      continue;
    }
    s.push_back(*p);
  }
  // 按逗号分
  std::vector<std::string> parts;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == ',') {
      if (i > start) parts.push_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  if (parts.empty()) return false;
  // 第一段 module+rva 或 0xaddr
  const std::string& head = parts[0];
  auto plus = head.find('+');
  if (plus != std::string::npos) {
    out.module = head.substr(0, plus);
    unsigned long long rva = 0;
    std::sscanf(head.c_str() + plus + 1, "%llx", &rva);
    out.base_rva = (uintptr_t)rva;
    ptrscan_rebind(out);  // 尝试定位 base
  } else {
    unsigned long long b = 0;
    if (std::sscanf(head.c_str(), "%llx", &b) != 1) return false;
    out.base = (uintptr_t)b;
  }
  for (size_t i = 1; i < parts.size(); ++i) {
    const char* p = parts[i].c_str();
    if (p[0] == '+') p++;
    unsigned off = 0;
    std::sscanf(p, "%x", &off);
    out.offsets.push_back((int32_t)off);
  }
  return !out.offsets.empty() || out.base != 0;
}

int ptrscan_verify_all(std::vector<PtrChain>& chains) {
  int ok = 0;
  for (auto& c : chains) {
    // 优先重绑模块
    if (!c.module.empty() && c.module != "anon") ptrscan_rebind(c);
    uintptr_t r = 0;
    if (ptrscan_resolve(c, r)) {
      c.resolved = r;
      c.valid = true;
      ok++;
    } else {
      c.valid = false;
      c.resolved = 0;
    }
  }
  return ok;
}

int ptrscan_save(const char* path, const std::vector<PtrChain>& chains) {
  if (!path) return -1;
  FILE* f = std::fopen(path, "w");
  if (!f) return -1;
  std::fprintf(f, "MEMDBG_PTR 1\n");
  int n = 0;
  for (auto& c : chains) {
    char line[512];
    ptrscan_format(c, line, sizeof(line));
    // 存可解析格式: module+rva,off,off
    if (!c.module.empty() && c.module != "anon") {
      std::fprintf(f, "%s+0x%llX", c.module.c_str(),
                   (unsigned long long)c.base_rva);
    } else {
      std::fprintf(f, "0x%llX", (unsigned long long)c.base);
    }
    for (int32_t off : c.offsets)
      std::fprintf(f, ",0x%X", (unsigned)off);
    std::fprintf(f, "\n");
    n++;
  }
  std::fclose(f);
  return n;
}

int ptrscan_load(const char* path, std::vector<PtrChain>& chains) {
  if (!path) return -1;
  FILE* f = std::fopen(path, "r");
  if (!f) return -1;
  char line[512];
  if (!std::fgets(line, sizeof(line), f) ||
      std::strncmp(line, "MEMDBG_PTR", 10) != 0) {
    std::fclose(f);
    return -1;
  }
  chains.clear();
  while (std::fgets(line, sizeof(line), f)) {
    size_t n = std::strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!line[0] || line[0] == '#') continue;
    PtrChain c;
    if (ptrscan_parse_template(line, c)) chains.push_back(c);
  }
  std::fclose(f);
  ptrscan_verify_all(chains);
  return (int)chains.size();
}

}  // namespace mem
