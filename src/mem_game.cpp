/**
 * 游戏向 / 自动化实现
 */
#include "mem_game.hpp"
#include "mem_bp.hpp"
#include "mem_core.hpp"
#include "mem_disasm.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <linux/input.h>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace mem {
namespace {

// ── 状态 ──────────────────────────────────────────────────
char g_speed_st[192] = "speed: off";
float g_speed_mult = 0.f;
bool g_speed_on = false;
uintptr_t g_speed_fn = 0;       // clock_gettime
uintptr_t g_speed_fn2 = 0;      // gettimeofday (可选)
uintptr_t g_speed_cave = 0;     // 可执行 cave
uint32_t g_speed_orig[4]{};     // clock_gettime 前 16 字节
uint32_t g_speed_orig2[4]{};    // gettimeofday 前 16 字节
bool g_speed_patched = false;
bool g_speed_hard = false;      // true=已写入代码钩子

char g_script_log[4096]{};

char g_aa_st[160] = "aa: idle";
struct AaSym {
  char name[48]{};
  uintptr_t addr = 0;
  size_t size = 0;
  bool is_alloc = false;
  bool is_patch = false;
  std::vector<uint8_t> backup;
};
std::vector<AaSym> g_aa_syms;
std::unordered_map<std::string, uintptr_t> g_aa_map;

std::mutex g_hk_mu;
std::vector<HotkeyBind> g_hotkeys;
std::vector<int> g_hk_fds;
char g_hk_msg[96]{};
bool g_hk_inited = false;

void log_append(const char* s) {
  if (!s) return;
  size_t n = std::strlen(g_script_log);
  size_t m = std::strlen(s);
  if (n + m + 2 >= sizeof(g_script_log)) {
    // 截断前半
    std::memmove(g_script_log, g_script_log + sizeof(g_script_log) / 2,
                 sizeof(g_script_log) / 2);
    n = std::strlen(g_script_log);
  }
  std::snprintf(g_script_log + n, sizeof(g_script_log) - n, "%s\n", s);
}

void set_err(char* err, size_t cap, const char* msg) {
  if (err && cap) std::snprintf(err, cap, "%s", msg ? msg : "error");
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) ++a;
  while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
  return s.substr(a, b - a);
}

std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

bool parse_hex_bytes(const char* pat, std::vector<uint8_t>& bytes,
                     std::vector<uint8_t>& mask) {
  bytes.clear();
  mask.clear();
  if (!pat) return false;
  const char* p = pat;
  while (*p) {
    while (*p && std::isspace((unsigned char)*p)) ++p;
    if (!*p) break;
    if (p[0] == '?' && (p[1] == '?' || p[1] == '\0' || std::isspace((unsigned char)p[1]))) {
      bytes.push_back(0);
      mask.push_back(0);
      p += (p[1] == '?') ? 2 : 1;
      continue;
    }
    char* end = nullptr;
    long v = std::strtol(p, &end, 16);
    if (end == p) return false;
    bytes.push_back((uint8_t)(v & 0xFF));
    mask.push_back(0xFF);
    p = end;
  }
  return !bytes.empty();
}

bool aob_find(const std::vector<uint8_t>& pat, const std::vector<uint8_t>& mask,
              const char* module_sub, uintptr_t& out) {
  out = 0;
  if (pat.empty()) return false;
  auto maps = load_maps(false);
  for (auto& r : maps) {
    if (!r.readable || r.perms[2] != 'x') continue;
    if (module_sub && module_sub[0]) {
      if (r.path.find(module_sub) == std::string::npos) continue;
    }
    size_t len = r.end - r.start;
    if (len < pat.size() || len > 64u * 1024 * 1024) continue;
    // 分块扫描
    const size_t chunk = 256 * 1024;
    std::vector<uint8_t> buf(chunk + pat.size());
    for (uintptr_t base = r.start; base + pat.size() <= r.end; base += chunk) {
      size_t want = std::min(chunk + pat.size(), (size_t)(r.end - base));
      if (!read_mem(base, buf.data(), want)) continue;
      for (size_t i = 0; i + pat.size() <= want; ++i) {
        bool ok = true;
        for (size_t j = 0; j < pat.size(); ++j) {
          if (mask[j] && buf[i + j] != pat[j]) {
            ok = false;
            break;
          }
        }
        if (ok) {
          out = base + i;
          return true;
        }
      }
    }
  }
  return false;
}

// 找可写槽（用于 cave / 数据）
uintptr_t find_writable_slot(size_t need) {
  if (need < 16) need = 16;
  auto maps = load_maps(true);
  if (maps.empty()) maps = load_maps(false);
  // 优先：大块匿名可写，取尾部（降低踩堆元数据概率）
  uintptr_t best = 0;
  size_t best_sz = 0;
  for (auto& r : maps) {
    if (!r.writable) continue;
    size_t sz = r.end > r.start ? (size_t)(r.end - r.start) : 0;
    if (sz < need + 0x200 || r.start < 0x10000) continue;
    if (r.path.find("stack") != std::string::npos) continue;
    bool anon = r.path.empty() || r.path[0] == '[';
    // 匿名优先
    if (anon && sz > best_sz) {
      best_sz = sz;
      best = r.end - need - 0x100;
    }
  }
  if (best) return best & ~7ull;
  // 回退：任意可写大段
  for (auto it = maps.rbegin(); it != maps.rend(); ++it) {
    auto& r = *it;
    if (!r.writable) continue;
    size_t sz = r.end > r.start ? (size_t)(r.end - r.start) : 0;
    if (sz < need + 0x200 || r.start < 0x10000) continue;
    if (r.path.find("stack") != std::string::npos) continue;
    return (r.end - need - 0x80) & ~7ull;
  }
  // 再退：段首 + 0x100
  for (auto& r : maps) {
    if (!r.writable || r.end - r.start < need + 0x200) continue;
    if (r.start < 0x10000) continue;
    return (r.start + 0x100) & ~7ull;
  }
  return 0;
}

// 从模块导出找符号（读 so 的 dynsym）
uintptr_t find_export_in_maps(const char* mod_sub, const char* name) {
  if (!name) return 0;
  auto maps = load_maps(false);
  std::string seen;
  for (auto& r : maps) {
    if (r.path.empty() || r.path[0] != '/') continue;
    if (mod_sub && r.path.find(mod_sub) == std::string::npos) continue;
    if (seen == r.path) continue;
    seen = r.path;
    // 用 path 打开 ELF
    FILE* f = std::fopen(r.path.c_str(), "rb");
    if (!f) continue;
    uint8_t eident[16];
    if (std::fread(eident, 1, 16, f) != 16 || eident[0] != 0x7f) {
      std::fclose(f);
      continue;
    }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    if (sz <= 0 || sz > 32 * 1024 * 1024) {
      std::fclose(f);
      continue;
    }
    std::rewind(f);
    std::vector<uint8_t> buf((size_t)sz);
    if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
      std::fclose(f);
      continue;
    }
    std::fclose(f);
    // minimal ELF64 dynsym walk (reuse logic simplified)
#pragma pack(push, 1)
    struct Ehdr {
      uint8_t i[16];
      uint16_t type, machine;
      uint32_t version;
      uint64_t entry, phoff, shoff;
      uint32_t flags;
      uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
    };
    struct Phdr {
      uint32_t type, flags;
      uint64_t offset, vaddr, paddr, filesz, memsz, align;
    };
    struct Dyn {
      int64_t tag;
      uint64_t val;
    };
    struct Sym {
      uint32_t name;
      uint8_t info, other;
      uint16_t shndx;
      uint64_t value, size;
    };
#pragma pack(pop)
    if (buf.size() < sizeof(Ehdr)) continue;
    auto* eh = (Ehdr*)buf.data();
    if (eh->i[4] != 2) continue;
    uint64_t min_v = UINT64_MAX;
    uint64_t dyn_off = 0, dyn_sz = 0;
    if (eh->phoff + (size_t)eh->phnum * sizeof(Phdr) > buf.size()) continue;
    auto* ph = (Phdr*)(buf.data() + eh->phoff);
    for (uint16_t i = 0; i < eh->phnum; ++i) {
      if (ph[i].type == 1 /*PT_LOAD*/ && ph[i].vaddr < min_v) min_v = ph[i].vaddr;
      if (ph[i].type == 2 /*PT_DYNAMIC*/) {
        dyn_off = ph[i].offset;
        dyn_sz = ph[i].filesz;
      }
    }
    if (min_v == UINT64_MAX || !dyn_off) continue;
    uintptr_t bias = r.start - (uintptr_t)min_v;
    auto v2o = [&](uint64_t v) -> int64_t {
      for (uint16_t i = 0; i < eh->phnum; ++i) {
        if (ph[i].type != 1) continue;
        if (v >= ph[i].vaddr && v < ph[i].vaddr + ph[i].filesz)
          return (int64_t)(ph[i].offset + (v - ph[i].vaddr));
      }
      return -1;
    };
    if (dyn_off + dyn_sz > buf.size()) continue;
    auto* d = (Dyn*)(buf.data() + dyn_off);
    size_t nd = dyn_sz / sizeof(Dyn);
    uint64_t str_v = 0, sym_v = 0, strsz = 0, syment = sizeof(Sym);
    for (size_t i = 0; i < nd; ++i) {
      if (d[i].tag == 0) break;
      if (d[i].tag == 5) str_v = d[i].val;
      else if (d[i].tag == 6) sym_v = d[i].val;
      else if (d[i].tag == 10) strsz = d[i].val;
      else if (d[i].tag == 11) syment = d[i].val;
    }
    int64_t so = v2o(str_v), yo = v2o(sym_v);
    if (so < 0 || yo < 0 || strsz == 0) continue;
    const char* strtab = (const char*)(buf.data() + so);
    for (int k = 0; k < 8192; ++k) {
      size_t off = (size_t)yo + (size_t)k * (size_t)syment;
      if (off + sizeof(Sym) > buf.size()) break;
      auto* sy = (Sym*)(buf.data() + off);
      if (sy->name >= strsz) continue;
      if (std::strcmp(strtab + sy->name, name) == 0 && sy->value) {
        return bias + (uintptr_t)sy->value;
      }
    }
  }
  return 0;
}

// ── Speedhack ─────────────────────────────────────────────
// 1) hard: remote mmap(RWX) + 钩 clock_gettime / gettimeofday（Δt * mult）
// 2) soft: 仅加速冻结写回（无代码补丁，永远可用）

static bool code_writable(uintptr_t addr) {
  uint32_t cur = 0, tmp = 0xD503201Fu;  // nop
  if (!read_mem(addr, &cur, 4)) return false;
  if (!write_mem(addr, &tmp, 4)) return false;
  write_mem(addr, &cur, 4);  // 还原
  return true;
}

static uintptr_t remote_mmap_rwx(size_t len) {
  if (len < 0x1000) len = 0x1000;
  uintptr_t mmap_fn = 0;
  char mod[128]{};
  if (!sym_find_by_name("mmap", mmap_fn, mod, sizeof(mod)) || !mmap_fn)
    sym_find_by_name("mmap64", mmap_fn, mod, sizeof(mod));
  if (!mmap_fn) mmap_fn = find_export_in_maps("libc.so", "mmap");
  if (!mmap_fn) return 0;
  // void* mmap(void*, size_t, int prot, int flags, int fd, off_t);
  // PROT_READ|WRITE|EXEC = 7, MAP_PRIVATE|ANONYMOUS = 0x22
  uint64_t args[6] = {0, (uint64_t)len, 7, 0x22, (uint64_t)-1, 0};
  uint64_t ret = 0;
  if (!remote_call(mmap_fn, args, 6, &ret)) return 0;
  if (!ret || ret > 0xFFFFFFFF00000000ull) return 0;  // MAP_FAILED ~ -1
  return (uintptr_t)ret;
}

// 绝对跳转 16 字节: LDR X16, #8; BR X16; .quad target
static void write_abs_jump(uint8_t* out, uintptr_t target) {
  // LDR X16, #8  => 0x58000050
  // BR  X16      => 0xD61F0200
  uint32_t ins[2] = {0x58000050u, 0xD61F0200u};
  std::memcpy(out, ins, 8);
  std::memcpy(out + 8, &target, 8);
}

// 更可靠：用「原函数 trampoline + 简单比例缩放」的紧凑 shellcode
// 若构造失败则仅 soft
static bool patch_fn_to_cave(uintptr_t fn, uintptr_t cave_entry,
                             uint32_t* saved_orig) {
  if (!read_mem(fn, saved_orig, 16)) return false;
  if (!code_writable(fn)) return false;
  uint8_t jmp[16];
  write_abs_jump(jmp, cave_entry);
  return write_mem(fn, jmp, 16);
}

bool install_clock_hook(float mult) {
  // 先卸旧钩
  if (g_speed_patched && g_speed_fn) {
    write_mem(g_speed_fn, g_speed_orig, 16);
    if (g_speed_fn2)
      write_mem(g_speed_fn2, g_speed_orig2, 16);
    g_speed_patched = false;
    g_speed_hard = false;
  }

  g_speed_mult = mult;
  g_speed_on = mult > 0.001f && std::fabs(mult - 1.f) > 0.001f;
  g_speed_fn = 0;
  g_speed_fn2 = 0;
  g_speed_cave = 0;
  g_speed_hard = false;

  if (!g_speed_on) {
    std::snprintf(g_speed_st, sizeof(g_speed_st), "speed: off");
    return true;
  }

  // 解析符号
  uintptr_t fn = 0, fn2 = 0;
  char mod[128]{};
  if (!sym_find_by_name("clock_gettime", fn, mod, sizeof(mod)) || !fn)
    sym_find_by_name("__clock_gettime", fn, mod, sizeof(mod));
  if (!fn) fn = find_export_in_maps("libc.so", "clock_gettime");
  if (!fn) fn = find_export_in_maps("bionic", "clock_gettime");
  sym_find_by_name("gettimeofday", fn2, mod, sizeof(mod));
  if (!fn2) fn2 = find_export_in_maps("libc.so", "gettimeofday");

  g_speed_fn = fn;
  g_speed_fn2 = fn2;

  // ── 尝试 hard hook（不用 remote_call/mmap，避免崩目标）──
  // 在 libc 可执行段内找连续 NOP/零填充作 cave
  bool hard_ok = false;
  if (fn && read_mem(fn, g_speed_orig, 16)) {
    bool can_code = code_writable(fn);
    uintptr_t cave = 0;
    if (can_code) {
      // 1) 已有 rwx
      auto maps = load_maps(false);
      for (auto& r : maps) {
        if (r.readable && r.writable && r.perms[2] == 'x' &&
            r.end - r.start >= 0x200) {
          cave = (r.start + 0x40) & ~0xFull;
          break;
        }
      }
      // 2) 在 fn 附近 ±2MB 的 r-x 段扫 0x100 连续 0 / NOP
      if (!cave) {
        uintptr_t base = fn & ~0xFFFull;
        for (int delta = 0x1000; delta < 0x200000 && !cave; delta += 0x1000) {
          for (int sign = -1; sign <= 1 && !cave; sign += 2) {
            uintptr_t reg = base + (uintptr_t)(sign * delta);
            uint8_t buf[0x120]{};
            if (!read_mem(reg, buf, sizeof(buf))) continue;
            for (size_t off = 0; off + 0x100 <= sizeof(buf); off += 16) {
              bool empty = true;
              for (size_t k = 0; k < 0x100; ++k) {
                if (buf[off + k] != 0 && buf[off + k] != 0x1F) {
                  // allow nop 1F2003D5 little = bytes 1f 20 03 d5
                  empty = false;
                  break;
                }
              }
              // 放宽：全 0 区
              empty = true;
              for (size_t k = 0; k < 0x100; ++k)
                if (buf[off + k] != 0) {
                  empty = false;
                  break;
                }
              if (empty) {
                cave = reg + off;
                break;
              }
            }
          }
        }
      }
    }

    if (cave && can_code) {
      g_speed_cave = cave;
      uint8_t page[0x100]{};
      std::memcpy(page + 0x80, g_speed_orig, 16);
      write_abs_jump(page + 0x90, fn + 16);
      double md = (double)mult;
      std::memcpy(page + 0xA0, &md, 8);
      int64_t zero = 0;
      std::memcpy(page + 0xA8, &zero, 8);
      std::memcpy(page + 0xB0, &zero, 8);
      uint32_t ready = 0;
      std::memcpy(page + 0xB8, &ready, 4);
      uintptr_t tramp = cave + 0x80;
      uintptr_t data = cave + 0xA0;
      std::memcpy(page + 0x70, &tramp, 8);
      std::memcpy(page + 0x78, &data, 8);

      uint32_t code[] = {
          0xA9BE7BFD, 0xA90107E0,
          0x58000000u | (0x1Au << 5) | 16, 0xD63F0200, 0xA94107E0,
          0xB4000000u | (22u << 5) | 1,
          0x58000000u | (0x18u << 5) | 16, 0xA9401023, 0xD28B5405, 0xF2A77305,
          0x9B054C66, 0xFD400200, 0xF9400607, 0xB9401A08, 0x35000088,
          0xF9000606, 0xF9000A06, 0x52800028, 0xB9001A08, 0xCB0700C9,
          0x9E630122, 0x1E620800, 0x9E79000A, 0xF9400A0B, 0x8B0A016A,
          0x9AC50803, 0x9B058064, 0xA9001023, 0xD2800000, 0xA8C27BFD,
          0xD65F03C0,
      };
      std::memcpy(page, code, sizeof(code));

      if (write_mem(cave, page, sizeof(page)) &&
          patch_fn_to_cave(fn, cave, g_speed_orig)) {
        hard_ok = true;
        g_speed_patched = true;
        g_speed_hard = true;
      }
    }
    (void)fn2;
  }

  if (hard_ok) {
    std::snprintf(g_speed_st, sizeof(g_speed_st),
                  "speed: %.2fx HARD @0x%llX cave@0x%llX", mult,
                  (unsigned long long)fn, (unsigned long long)g_speed_cave);
    return true;
  }

  // soft 永远可用：加速冻结写回 + 状态明确
  std::snprintf(g_speed_st, sizeof(g_speed_st),
                "speed: %.2fx SOFT · 冻结加速已开%s", mult,
                fn ? "" : " (无clock符号)");
  return true;
}

ValType parse_vtype(const std::string& t) {
  std::string s = lower(t);
  if (s == "i8" || s == "byte") return ValType::I8;
  if (s == "i16" || s == "short") return ValType::I16;
  if (s == "i32" || s == "int") return ValType::I32;
  if (s == "i64" || s == "long") return ValType::I64;
  if (s == "f32" || s == "float") return ValType::F32;
  if (s == "f64" || s == "double") return ValType::F64;
  return ValType::I32;
}

// ── 脚本执行 ──────────────────────────────────────────────

// (MDS removed — scripts are Lua in mem_lua.cpp)

// ── Auto Assemble ─────────────────────────────────────────
uintptr_t aa_get(const std::string& name) {
  auto it = g_aa_map.find(name);
  if (it != g_aa_map.end()) return it->second;
  // 尝试解析为地址
  uintptr_t a = 0;
  if (parse_addr(name.c_str(), a)) return a;
  return 0;
}

void aa_set(const std::string& name, uintptr_t addr, size_t sz, bool alloc,
            bool patch, const std::vector<uint8_t>& bak) {
  g_aa_map[name] = addr;
  for (auto& s : g_aa_syms) {
    if (name == s.name) {
      s.addr = addr;
      s.size = sz;
      s.is_alloc = alloc;
      s.is_patch = patch;
      if (!bak.empty()) s.backup = bak;
      return;
    }
  }
  AaSym s;
  std::snprintf(s.name, sizeof(s.name), "%s", name.c_str());
  s.addr = addr;
  s.size = sz;
  s.is_alloc = alloc;
  s.is_patch = patch;
  s.backup = bak;
  g_aa_syms.push_back(std::move(s));
}

bool aa_enable_block(const std::string& body, char* err, size_t err_cap) {
  std::istringstream in(body);
  std::string line;
  uintptr_t cursor = 0;  // 当前 alloc 写入点
  std::string cur_alloc;

  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '/' || line[0] == ';') continue;
    // aobscan(sym, bytes)
    if (line.find("aobscanmodule(") == 0 || line.find("aobscan(") == 0) {
      bool has_mod = line.find("aobscanmodule(") == 0;
      size_t lp = line.find('('), rp = line.rfind(')');
      if (lp == std::string::npos || rp == std::string::npos) {
        set_err(err, err_cap, "aobscan syntax");
        return false;
      }
      std::string args = line.substr(lp + 1, rp - lp - 1);
      // split by comma
      std::vector<std::string> parts;
      {
        std::string cur;
        int depth = 0;
        for (char c : args) {
          if (c == ',' && depth == 0) {
            parts.push_back(trim(cur));
            cur.clear();
          } else {
            if (c == '(') depth++;
            if (c == ')') depth--;
            cur.push_back(c);
          }
        }
        if (!cur.empty()) parts.push_back(trim(cur));
      }
      std::string sym, mod, pat;
      if (has_mod) {
        if (parts.size() < 3) {
          set_err(err, err_cap, "aobscanmodule need 3 args");
          return false;
        }
        sym = parts[0];
        mod = parts[1];
        pat = parts[2];
      } else {
        if (parts.size() < 2) {
          set_err(err, err_cap, "aobscan need 2 args");
          return false;
        }
        sym = parts[0];
        pat = parts[1];
      }
      std::vector<uint8_t> b, m;
      if (!parse_hex_bytes(pat.c_str(), b, m)) {
        set_err(err, err_cap, "aob pattern");
        return false;
      }
      uintptr_t hit = 0;
      if (!aob_find(b, m, mod.empty() ? nullptr : mod.c_str(), hit)) {
        set_err(err, err_cap, "aobscan not found");
        return false;
      }
      aa_set(sym, hit, b.size(), false, false, {});
      char msg[80];
      std::snprintf(msg, sizeof(msg), "aob %s=0x%llX", sym.c_str(),
                    (unsigned long long)hit);
      log_append(msg);
      continue;
    }
    if (line.find("alloc(") == 0) {
      size_t lp = line.find('('), rp = line.rfind(')');
      std::string args = line.substr(lp + 1, rp - lp - 1);
      size_t c = args.find(',');
      std::string sym = trim(args.substr(0, c));
      size_t sz = 256;
      if (c != std::string::npos) sz = (size_t)std::strtoul(args.c_str() + c + 1, nullptr, 0);
      if (sz < 16) sz = 16;
      if (sz > 0x10000) sz = 0x10000;
      uintptr_t slot = find_writable_slot(sz + 64);
      if (!slot) {
        set_err(err, err_cap, "alloc: no writable slot");
        return false;
      }
      // 清零
      std::vector<uint8_t> z(sz, 0);
      write_mem(slot, z.data(), z.size());
      aa_set(sym, slot, sz, true, false, {});
      cursor = slot;
      cur_alloc = sym;
      continue;
    }
    if (line.find("label(") == 0) {
      size_t lp = line.find('('), rp = line.rfind(')');
      std::string sym = trim(line.substr(lp + 1, rp - lp - 1));
      if (cursor)
        aa_set(sym, cursor, 0, false, false, {});
      continue;
    }
    if (line.find("registersymbol(") == 0) {
      // already in map
      continue;
    }
    if (line.find("writebytes(") == 0 || line.find("db ") == 0 ||
        line.find("db(") == 0) {
      std::string target;
      std::string hex;
      if (line.find("writebytes(") == 0) {
        size_t lp = line.find('('), rp = line.rfind(')');
        std::string args = line.substr(lp + 1, rp - lp - 1);
        size_t c = args.find(',');
        target = trim(args.substr(0, c));
        hex = trim(args.substr(c + 1));
      } else {
        // db inside alloc: db XX XX
        hex = trim(line.substr(2));
        if (hex[0] == '(') {
          size_t rp = hex.rfind(')');
          hex = hex.substr(1, rp - 1);
        }
        target.clear();
      }
      std::vector<uint8_t> b, m;
      if (!parse_hex_bytes(hex.c_str(), b, m)) {
        set_err(err, err_cap, "writebytes bad hex");
        return false;
      }
      uintptr_t addr = target.empty() ? cursor : aa_get(target);
      if (!addr) {
        set_err(err, err_cap, "writebytes: bad addr/sym");
        return false;
      }
      std::vector<uint8_t> bak(b.size());
      read_mem(addr, bak.data(), bak.size());
      if (!write_mem(addr, b.data(), b.size())) {
        set_err(err, err_cap, "writebytes: write fail");
        return false;
      }
      if (!target.empty())
        aa_set(target + "_bak", addr, b.size(), false, true, bak);
      else {
        // patch at cursor
        char nm[48];
        std::snprintf(nm, sizeof(nm), "patch_%llx", (unsigned long long)addr);
        aa_set(nm, addr, b.size(), false, true, bak);
        cursor = addr + b.size();
        if (!cur_alloc.empty()) g_aa_map[cur_alloc] = g_aa_map[cur_alloc];  // keep
      }
      continue;
    }
    if (line.find("nop(") == 0) {
      size_t lp = line.find('('), rp = line.rfind(')');
      std::string args = line.substr(lp + 1, rp - lp - 1);
      size_t c = args.find(',');
      std::string target = trim(args.substr(0, c));
      int n = 1;
      if (c != std::string::npos)
        n = (int)std::strtol(args.c_str() + c + 1, nullptr, 0);
      uintptr_t addr = aa_get(target);
      if (!addr) {
        set_err(err, err_cap, "nop: bad sym");
        return false;
      }
      std::vector<uint8_t> bak((size_t)n * 4);
      read_mem(addr, bak.data(), bak.size());
      if (!patch_nop(addr, n)) {
        set_err(err, err_cap, "nop fail");
        return false;
      }
      aa_set(target + "_nopbak", addr, bak.size(), false, true, bak);
      continue;
    }
    // 行内：sym:  或 汇编
    if (!line.empty() && line.back() == ':') {
      std::string sym = trim(line.substr(0, line.size() - 1));
      if (cursor) aa_set(sym, cursor, 0, false, false, {});
      continue;
    }
    // 尝试当作汇编写入 cursor
    if (cursor) {
      uint32_t w = 0;
      char e2[64];
      if (assemble_line(line.c_str(), w, e2, sizeof(e2))) {
        std::vector<uint8_t> bak(4);
        read_mem(cursor, bak.data(), 4);
        write_mem(cursor, &w, 4);
        char nm[48];
        std::snprintf(nm, sizeof(nm), "asm_%llx", (unsigned long long)cursor);
        aa_set(nm, cursor, 4, false, true, bak);
        cursor += 4;
        continue;
      }
    }
    // 忽略 fullaccess 等
    if (line.find("fullaccess") != std::string::npos) continue;
    // 未知行
    char msg[96];
    std::snprintf(msg, sizeof(msg), "aa skip: %.60s", line.c_str());
    log_append(msg);
  }
  std::snprintf(g_aa_st, sizeof(g_aa_st), "aa enable ok, syms=%d",
                (int)g_aa_map.size());
  return true;
}

bool aa_disable_block(const std::string& body, char* err, size_t err_cap) {
  (void)body;
  // 还原所有 patch backup，释放 alloc 标记
  for (auto it = g_aa_syms.rbegin(); it != g_aa_syms.rend(); ++it) {
    if (it->is_patch && !it->backup.empty()) {
      write_mem(it->addr, it->backup.data(), it->backup.size());
    }
  }
  // 不主动清 map，允许再次 enable；完整清理：
  g_aa_syms.clear();
  g_aa_map.clear();
  std::snprintf(g_aa_st, sizeof(g_aa_st), "aa disabled");
  (void)err;
  (void)err_cap;
  return true;
}

// ── 热键 ──────────────────────────────────────────────────
bool open_key_devices() {
  for (int fd : g_hk_fds)
    if (fd >= 0) close(fd);
  g_hk_fds.clear();
  DIR* d = opendir("/dev/input");
  if (!d) return false;
  dirent* e;
  while ((e = readdir(d))) {
    if (std::strncmp(e->d_name, "event", 5) != 0) continue;
    char path[64];
    std::snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) continue;
    // 只要有 EV_KEY
    unsigned long evbits[(EV_MAX + 64) / (8 * sizeof(long))]{};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) {
      close(fd);
      continue;
    }
    auto test_bit = [&](int bit) {
      return evbits[bit / (8 * sizeof(long))] &
             (1UL << (bit % (8 * sizeof(long))));
    };
    if (!test_bit(EV_KEY)) {
      close(fd);
      continue;
    }
    g_hk_fds.push_back(fd);
  }
  closedir(d);
  return !g_hk_fds.empty();
}

void dispatch_hotkey(HotkeyBind& hk) {
  hk.hit_count++;
  std::string act = lower(hk.action);
  if (act == "speed") {
    float f = (float)std::atof(hk.arg);
    if (f <= 0.f) speed_disable();
    else speed_set(f);
    std::snprintf(g_hk_msg, sizeof(g_hk_msg), "hotkey speed %s", speed_status());
  } else if (act == "freeze_all") {
    // 无法直接访问 UI 表；用 scan freeze 无统一
    std::snprintf(g_hk_msg, sizeof(g_hk_msg), "hotkey freeze_all (use script)");
  } else if (act == "script") {
    char e[128];
    if (script_run_file(hk.arg, e, sizeof(e)))
      std::snprintf(g_hk_msg, sizeof(g_hk_msg), "hotkey script ok");
    else
      std::snprintf(g_hk_msg, sizeof(g_hk_msg), "hotkey script: %s", e);
  } else if (act == "script_text") {
    char e[128];
    if (script_run(hk.arg, e, sizeof(e)))
      std::snprintf(g_hk_msg, sizeof(g_hk_msg), "hotkey ok");
    else
      std::snprintf(g_hk_msg, sizeof(g_hk_msg), "hotkey: %s", e);
  } else if (act == "nop_pc") {
    Regs r;
    if (dbg_regs_read(r) && r.pc)
      patch_nop(r.pc, 1);
    std::snprintf(g_hk_msg, sizeof(g_hk_msg), "hotkey nop_pc");
  } else {
    std::snprintf(g_hk_msg, sizeof(g_hk_msg), "hotkey %s", hk.action);
  }
}

}  // namespace

// ── public speed ──────────────────────────────────────────
bool speed_set(float mult) {
  if (mult <= 0.001f) {
    speed_disable();
    return true;
  }
  if (!is_attached()) {
    std::snprintf(g_speed_st, sizeof(g_speed_st), "speed: 未附加");
    return false;
  }
  return install_clock_hook(mult);
}

float speed_get() { return g_speed_on ? g_speed_mult : 0.f; }
bool speed_active() { return g_speed_on; }

void speed_disable() {
  if (g_speed_patched && g_speed_fn) {
    write_mem(g_speed_fn, g_speed_orig, 16);
  }
  if (g_speed_patched && g_speed_fn2) {
    write_mem(g_speed_fn2, g_speed_orig2, 16);
  }
  g_speed_on = false;
  g_speed_mult = 0.f;
  g_speed_patched = false;
  g_speed_hard = false;
  g_speed_fn = 0;
  g_speed_fn2 = 0;
  // cave 页保留（进程内匿名映射），不 munmap 以免复杂
  g_speed_cave = 0;
  std::snprintf(g_speed_st, sizeof(g_speed_st), "speed: off");
}

const char* speed_status() { return g_speed_st; }

// ── hotkey ────────────────────────────────────────────────
bool hotkey_init() {
  std::lock_guard<std::mutex> lk(g_hk_mu);
  g_hk_inited = open_key_devices();
  if (!g_hk_inited)
    std::snprintf(g_hk_msg, sizeof(g_hk_msg), "hotkey: 无法打开 /dev/input");
  else
    std::snprintf(g_hk_msg, sizeof(g_hk_msg), "hotkey: %d devices",
                  (int)g_hk_fds.size());
  return g_hk_inited;
}

void hotkey_shutdown() {
  std::lock_guard<std::mutex> lk(g_hk_mu);
  for (int fd : g_hk_fds)
    if (fd >= 0) close(fd);
  g_hk_fds.clear();
  g_hk_inited = false;
}

const char* hotkey_poll() {
  g_hk_msg[0] = 0;
  if (!g_hk_inited) return "";
  std::lock_guard<std::mutex> lk(g_hk_mu);
  input_event ev{};
  for (int fd : g_hk_fds) {
    while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
      if (ev.type != EV_KEY) continue;
      if (ev.value != 1) continue;  // press only
      for (auto& hk : g_hotkeys) {
        if (!hk.enabled) continue;
        if (hk.keycode == (int)ev.code) {
          dispatch_hotkey(hk);
          return g_hk_msg;
        }
      }
    }
  }
  return g_hk_msg[0] ? g_hk_msg : "";
}

void hotkey_clear() {
  std::lock_guard<std::mutex> lk(g_hk_mu);
  g_hotkeys.clear();
}

bool hotkey_add(int keycode, const char* action, const char* arg) {
  if (keycode <= 0 || !action) return false;
  std::lock_guard<std::mutex> lk(g_hk_mu);
  HotkeyBind h;
  h.keycode = keycode;
  std::snprintf(h.action, sizeof(h.action), "%s", action);
  if (arg) std::snprintf(h.arg, sizeof(h.arg), "%s", arg);
  g_hotkeys.push_back(h);
  if (!g_hk_inited) hotkey_init();
  return true;
}

bool hotkey_remove(int index) {
  std::lock_guard<std::mutex> lk(g_hk_mu);
  if (index < 0 || index >= (int)g_hotkeys.size()) return false;
  g_hotkeys.erase(g_hotkeys.begin() + index);
  return true;
}

std::vector<HotkeyBind> hotkey_list() {
  std::lock_guard<std::mutex> lk(g_hk_mu);
  return g_hotkeys;
}

int hotkey_parse_key(const char* name) {
  if (!name || !name[0]) return -1;
  std::string s = lower(name);
  // 去 KEY_ 前缀
  if (s.rfind("key_", 0) == 0) s = s.substr(4);
  if (s == "volup" || s == "volumeup" || s == "volume_up") return KEY_VOLUMEUP;
  if (s == "voldown" || s == "volumedown" || s == "volume_down")
    return KEY_VOLUMEDOWN;
  if (s == "power") return KEY_POWER;
  if (s == "home") return KEY_HOMEPAGE;
  if (s == "back") return KEY_BACK;
  if (s == "enter" || s == "ok") return KEY_ENTER;
  if (s == "space") return KEY_SPACE;
  if (s.size() == 1 && s[0] >= 'a' && s[0] <= 'z')
    return KEY_A + (s[0] - 'a');
  if (s.size() == 1 && s[0] >= '0' && s[0] <= '9')
    return KEY_0 + (s[0] - '0');
  // 数字 keycode
  if (std::isdigit((unsigned char)s[0])) return std::atoi(s.c_str());
  return -1;
}

const char* hotkey_key_name(int keycode) {
  switch (keycode) {
    case KEY_VOLUMEUP:
      return "VOL+";
    case KEY_VOLUMEDOWN:
      return "VOL-";
    case KEY_POWER:
      return "POWER";
    case KEY_BACK:
      return "BACK";
    case KEY_ENTER:
      return "ENTER";
    case KEY_SPACE:
      return "SPACE";
    default:
      break;
  }
  static char buf[16];
  std::snprintf(buf, sizeof(buf), "KEY_%d", keycode);
  return buf;
}

// ── AA ────────────────────────────────────────────────────
bool aa_run(const char* text, bool enable, char* err, size_t err_cap) {
  if (!text) {
    set_err(err, err_cap, "null aa");
    return false;
  }
  if (!is_attached()) {
    set_err(err, err_cap, "未附加");
    return false;
  }
  // 拆 ENABLE/DISABLE 段
  std::string t(text);
  std::string up = t;
  // 找段
  auto pos_en = lower(t).find("[enable]");
  auto pos_dis = lower(t).find("[disable]");
  std::string body;
  if (enable) {
    if (pos_en != std::string::npos) {
      size_t start = pos_en + 8;
      size_t end = (pos_dis != std::string::npos && pos_dis > pos_en)
                       ? pos_dis
                       : t.size();
      body = t.substr(start, end - start);
    } else {
      body = t;  // 整段当 enable
    }
    return aa_enable_block(body, err, err_cap);
  } else {
    if (pos_dis != std::string::npos)
      body = t.substr(pos_dis + 9);
    return aa_disable_block(body, err, err_cap);
  }
}

void aa_disable_all() {
  char e[64];
  aa_disable_block("", e, sizeof(e));
}

const char* aa_status() { return g_aa_st; }
int aa_symbol_count() { return (int)g_aa_map.size(); }

bool aa_resolve(const char* sym, uintptr_t& out) {
  if (!sym) return false;
  auto it = g_aa_map.find(sym);
  if (it == g_aa_map.end()) return false;
  out = it->second;
  return true;
}

// ── Trainer ───────────────────────────────────────────────
int trainer_export(const AddressTable& table, const char* path,
                   const TrainerMeta& meta, const char* boot_script) {
  if (!path) return -1;
  FILE* f = std::fopen(path, "w");
  if (!f) return -1;
  std::fprintf(f, "# MemDbg Trainer v1\n");
  std::fprintf(f, "name=%s\n", meta.name);
  std::fprintf(f, "package=%s\n", meta.package);
  std::fprintf(f, "author=%s\n", meta.author);
  std::fprintf(f, "note=%s\n", meta.note);
  std::fprintf(f, "\n[entries]\n");
  int n = 0;
  for (auto& e : table.entries) {
    std::fprintf(f, "0x%llX %d %d 0x%llX %s\n", (unsigned long long)e.addr,
                 (int)e.type, e.freeze ? 1 : 0, (unsigned long long)e.freeze_bits,
                 e.desc);
    n++;
  }
  if (boot_script && boot_script[0]) {
    std::fprintf(f, "\n[script]\n%s\n", boot_script);
  }
  std::fclose(f);
  return n;
}

int trainer_export_sh(const AddressTable& table, const char* path,
                      const TrainerMeta& meta) {
  if (!path) return -1;
  FILE* f = std::fopen(path, "w");
  if (!f) return -1;
  std::fprintf(f, "#!/system/bin/sh\n");
  std::fprintf(f, "# Trainer: %s by %s\n", meta.name, meta.author);
  std::fprintf(f, "# package: %s\n", meta.package);
  std::fprintf(f, "# 简易 freeze 循环需要 root；推荐用 MemDbg 加载 .trainer\n");
  std::fprintf(f, "echo 'MemDbg Trainer: %s'\n", meta.name);
  std::fprintf(f, "echo 'Entries: %d — 请用 MemDbg 地址表导入同名 .trainer'\n",
               (int)table.entries.size());
  for (auto& e : table.entries) {
    std::fprintf(f, "echo '  0x%llX %s freeze=%d'\n",
                 (unsigned long long)e.addr, e.desc, e.freeze ? 1 : 0);
  }
  std::fclose(f);
  chmod(path, 0755);
  return (int)table.entries.size();
}

int trainer_import(AddressTable& table, const char* path, TrainerMeta* meta_out) {
  if (!path) return -1;
  FILE* f = std::fopen(path, "r");
  if (!f) return -1;
  TrainerMeta meta{};
  char line[512];
  enum { H, E, S } sec = H;
  std::string script;
  int n = 0;
  table.clear();
  while (std::fgets(line, sizeof(line), f)) {
    char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '#' || *p == '\n' || *p == '\r') continue;
    if (std::strncmp(p, "[entries]", 9) == 0) {
      sec = E;
      continue;
    }
    if (std::strncmp(p, "[script]", 8) == 0) {
      sec = S;
      continue;
    }
    if (sec == H) {
      if (std::strncmp(p, "name=", 5) == 0)
        std::snprintf(meta.name, sizeof(meta.name), "%s", trim(p + 5).c_str());
      else if (std::strncmp(p, "package=", 8) == 0)
        std::snprintf(meta.package, sizeof(meta.package), "%s",
                      trim(p + 8).c_str());
      else if (std::strncmp(p, "author=", 7) == 0)
        std::snprintf(meta.author, sizeof(meta.author), "%s",
                      trim(p + 7).c_str());
      else if (std::strncmp(p, "note=", 5) == 0)
        std::snprintf(meta.note, sizeof(meta.note), "%s", trim(p + 5).c_str());
      // also allow plain ct lines in header-less files
      unsigned long long addr = 0, bits = 0;
      int type = 0, fr = 0;
      char desc[64]{};
      if (std::sscanf(p, "0x%llx %d %d 0x%llx %63[^\n]", &addr, &type, &fr,
                      &bits, desc) >= 4) {
        TableEntry e;
        e.addr = (uintptr_t)addr;
        e.type = (ValType)type;
        e.freeze = fr != 0;
        e.freeze_bits = bits;
        if (desc[0]) std::snprintf(e.desc, sizeof(e.desc), "%s", desc);
        table.entries.push_back(e);
        n++;
      }
    } else if (sec == E) {
      unsigned long long addr = 0, bits = 0;
      int type = 0, fr = 0;
      char desc[64]{};
      if (std::sscanf(p, "0x%llx %d %d 0x%llx %63[^\n]", &addr, &type, &fr,
                      &bits, desc) >= 4) {
        TableEntry e;
        e.addr = (uintptr_t)addr;
        e.type = (ValType)type;
        e.freeze = fr != 0;
        e.freeze_bits = bits;
        if (desc[0]) std::snprintf(e.desc, sizeof(e.desc), "%s", desc);
        table.entries.push_back(e);
        n++;
      }
    } else if (sec == S) {
      script += p;
    }
  }
  std::fclose(f);
  if (meta_out) *meta_out = meta;
  if (!script.empty()) {
    char e[128];
    script_run(script.c_str(), e, sizeof(e));
  }
  return n;
}

}  // namespace mem
