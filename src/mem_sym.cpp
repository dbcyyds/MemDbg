/**
 * 从附加进程 maps 解析 ELF 动态符号 / 导出函数名
 */
#include "mem_disasm.hpp"
#include "mem_core.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace mem {
namespace {

struct ElfSym {
  uintptr_t addr = 0;
  size_t size = 0;
  std::string name;
  std::string module;
};

std::vector<ElfSym> g_syms;
std::unordered_map<uintptr_t, size_t> g_exact;  // addr -> index
bool g_loaded = false;

#pragma pack(push, 1)
struct Ehdr64 {
  uint8_t e_ident[16];
  uint16_t e_type, e_machine;
  uint32_t e_version;
  uint64_t e_entry, e_phoff, e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct Phdr64 {
  uint32_t p_type, p_flags;
  uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};
struct Shdr64 {
  uint32_t sh_name, sh_type;
  uint64_t sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info,
      sh_addralign, sh_entsize;
};
struct Sym64 {
  uint32_t st_name;
  uint8_t st_info, st_other;
  uint16_t st_shndx;
  uint64_t st_value, st_size;
};
struct Dyn64 {
  int64_t d_tag;
  uint64_t d_val;
};
#pragma pack(pop)

constexpr uint32_t PT_LOAD = 1;
constexpr uint32_t PT_DYNAMIC = 2;
constexpr uint32_t SHT_SYMTAB = 2;
constexpr uint32_t SHT_DYNSYM = 11;
constexpr int64_t DT_STRTAB = 5;
constexpr int64_t DT_SYMTAB = 6;
constexpr int64_t DT_STRSZ = 10;
constexpr int64_t DT_SYMENT = 11;
constexpr uint8_t STT_FUNC = 2;
constexpr uint8_t STT_OBJECT = 1;
constexpr uint8_t STT_GNU_IFUNC = 10;

bool read_file(const char* path, std::vector<uint8_t>& out, size_t maxn = 8u << 20) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  if (std::fseek(f, 0, SEEK_END) != 0) {
    std::fclose(f);
    return false;
  }
  long sz = std::ftell(f);
  if (sz <= 0 || (size_t)sz > maxn) {
    std::fclose(f);
    return false;
  }
  std::rewind(f);
  out.resize((size_t)sz);
  size_t n = std::fread(out.data(), 1, out.size(), f);
  std::fclose(f);
  out.resize(n);
  return n >= sizeof(Ehdr64);
}

bool parse_elf_buf(const uint8_t* data, size_t len, uintptr_t load_bias,
                   const char* modname, std::vector<ElfSym>& out) {
  if (!data || len < sizeof(Ehdr64)) return false;
  const Ehdr64* eh = reinterpret_cast<const Ehdr64*>(data);
  if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' ||
      eh->e_ident[3] != 'F')
    return false;
  if (eh->e_ident[4] != 2) return false;  // ELFCLASS64
  if (eh->e_machine != 183 /* EM_AARCH64 */ && eh->e_machine != 0xB7) return false;

  auto add_sym = [&](const char* name, uint64_t value, uint64_t size, uint8_t type) {
    if (!name || !name[0]) return;
    if (value == 0) return;
    if (type != STT_FUNC && type != STT_OBJECT && type != STT_GNU_IFUNC &&
        type != 0)
      return;
    // 跳过无意义
    if (name[0] == '$') return;
    ElfSym s;
    s.addr = load_bias + (uintptr_t)value;
    s.size = (size_t)size;
    s.name = name;
    s.module = modname ? modname : "";
    out.push_back(std::move(s));
  };

  // 优先 section: SHT_DYNSYM / SHT_SYMTAB
  if (eh->e_shoff && eh->e_shnum && eh->e_shentsize >= sizeof(Shdr64)) {
    if (eh->e_shoff + (size_t)eh->e_shnum * eh->e_shentsize <= len) {
      const Shdr64* sh =
          reinterpret_cast<const Shdr64*>(data + eh->e_shoff);
      for (uint16_t i = 0; i < eh->e_shnum; ++i) {
        if (sh[i].sh_type != SHT_DYNSYM && sh[i].sh_type != SHT_SYMTAB) continue;
        if (sh[i].sh_entsize < sizeof(Sym64)) continue;
        if (sh[i].sh_offset + sh[i].sh_size > len) continue;
        uint32_t link = (uint32_t)sh[i].sh_link;
        if (link >= eh->e_shnum) continue;
        if (sh[link].sh_offset + sh[link].sh_size > len) continue;
        const char* strtab =
            reinterpret_cast<const char*>(data + sh[link].sh_offset);
        size_t strsz = (size_t)sh[link].sh_size;
        size_t nsym = (size_t)(sh[i].sh_size / sh[i].sh_entsize);
        const uint8_t* base = data + sh[i].sh_offset;
        for (size_t k = 0; k < nsym; ++k) {
          const Sym64* sy =
              reinterpret_cast<const Sym64*>(base + k * sh[i].sh_entsize);
          if (sy->st_name >= strsz) continue;
          uint8_t type = sy->st_info & 0xF;
          add_sym(strtab + sy->st_name, sy->st_value, sy->st_size, type);
        }
      }
    }
  }

  // 回退：PT_DYNAMIC
  if (eh->e_phoff && eh->e_phnum) {
    if (eh->e_phoff + (size_t)eh->e_phnum * eh->e_phentsize <= len) {
      const Phdr64* ph =
          reinterpret_cast<const Phdr64*>(data + eh->e_phoff);
      uint64_t dyn_off = 0, dyn_sz = 0;
      uint64_t min_vaddr = UINT64_MAX;
      for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr < min_vaddr)
          min_vaddr = ph[i].p_vaddr;
        if (ph[i].p_type == PT_DYNAMIC) {
          dyn_off = ph[i].p_offset;
          dyn_sz = ph[i].p_filesz;
        }
      }
      (void)min_vaddr;
      if (dyn_off && dyn_sz && dyn_off + dyn_sz <= len) {
        const Dyn64* d = reinterpret_cast<const Dyn64*>(data + dyn_off);
        size_t nd = dyn_sz / sizeof(Dyn64);
        uint64_t strtab_v = 0, symtab_v = 0, strsz = 0, syment = sizeof(Sym64);
        for (size_t i = 0; i < nd; ++i) {
          if (d[i].d_tag == 0) break;
          if (d[i].d_tag == DT_STRTAB) strtab_v = d[i].d_val;
          else if (d[i].d_tag == DT_SYMTAB) symtab_v = d[i].d_val;
          else if (d[i].d_tag == DT_STRSZ) strsz = d[i].d_val;
          else if (d[i].d_tag == DT_SYMENT) syment = d[i].d_val;
        }
        // vaddr → file offset via PT_LOAD
        auto v2off = [&](uint64_t v) -> int64_t {
          for (uint16_t i = 0; i < eh->e_phnum; ++i) {
            if (ph[i].p_type != PT_LOAD) continue;
            if (v >= ph[i].p_vaddr && v < ph[i].p_vaddr + ph[i].p_filesz)
              return (int64_t)(ph[i].p_offset + (v - ph[i].p_vaddr));
          }
          return -1;
        };
        int64_t stro = v2off(strtab_v);
        int64_t symo = v2off(symtab_v);
        if (stro >= 0 && symo >= 0 && syment >= sizeof(Sym64) && strsz > 0) {
          if ((size_t)stro + strsz <= len) {
            const char* strtab = reinterpret_cast<const char*>(data + stro);
            // 扫描最多 4096 个符号
            for (int k = 0; k < 4096; ++k) {
              size_t off = (size_t)symo + (size_t)k * (size_t)syment;
              if (off + sizeof(Sym64) > len) break;
              const Sym64* sy = reinterpret_cast<const Sym64*>(data + off);
              if (sy->st_name == 0 && sy->st_value == 0 && k > 0) {
                // 可能结束；继续一点
                if (k > 8 && sy->st_size == 0) break;
              }
              if (sy->st_name >= strsz) continue;
              uint8_t type = sy->st_info & 0xF;
              add_sym(strtab + sy->st_name, sy->st_value, sy->st_size, type);
            }
          }
        }
      }
    }
  }
  return true;
}

const char* basename_of(const std::string& path) {
  auto p = path.find_last_of('/');
  if (p == std::string::npos) return path.c_str();
  return path.c_str() + p + 1;
}

}  // namespace

int sym_refresh() {
  g_syms.clear();
  g_exact.clear();
  g_loaded = false;
  if (!is_attached()) return 0;

  auto maps = load_maps(false);
  // 每个 path 只取 offset=0 的最低 r-x 映射作 base
  struct Mod {
    uintptr_t base = 0;
    std::string path;
  };
  std::vector<Mod> mods;
  for (auto& r : maps) {
    if (!r.readable) continue;
    if (r.path.empty() || r.path[0] != '/') continue;
    // 只要 so / 可执行 / apk 内 so
    bool ok = false;
    if (r.path.find(".so") != std::string::npos) ok = true;
    if (r.path.find("app_process") != std::string::npos) ok = true;
    if (r.perms[2] == 'x' && r.path.find("/bin/") != std::string::npos) ok = true;
    if (!ok) continue;
    // 仅第一段（通常 file offset 0 映射在 path 首次出现）
    bool seen = false;
    for (auto& m : mods) {
      if (m.path == r.path) {
        seen = true;
        break;
      }
    }
    if (seen) continue;
    if (r.perms[2] != 'x' && r.path.find(".so") == std::string::npos) continue;
    mods.push_back({r.start, r.path});
  }

  for (auto& m : mods) {
    std::vector<uint8_t> buf;
    if (!read_file(m.path.c_str(), buf)) continue;
    // load bias: runtime base - min PT_LOAD vaddr
    uintptr_t bias = m.base;
    if (buf.size() >= sizeof(Ehdr64)) {
      const Ehdr64* eh = reinterpret_cast<const Ehdr64*>(buf.data());
      if (eh->e_phoff && eh->e_phnum &&
          eh->e_phoff + (size_t)eh->e_phnum * sizeof(Phdr64) <= buf.size()) {
        const Phdr64* ph =
            reinterpret_cast<const Phdr64*>(buf.data() + eh->e_phoff);
        uint64_t min_v = UINT64_MAX;
        for (uint16_t i = 0; i < eh->e_phnum; ++i) {
          if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr < min_v)
            min_v = ph[i].p_vaddr;
        }
        if (min_v != UINT64_MAX) bias = m.base - (uintptr_t)min_v;
      }
    }
    size_t before = g_syms.size();
    parse_elf_buf(buf.data(), buf.size(), bias, basename_of(m.path), g_syms);
    (void)before;
  }

  // 排序 + exact 索引
  std::sort(g_syms.begin(), g_syms.end(),
            [](const ElfSym& a, const ElfSym& b) { return a.addr < b.addr; });
  // 去重：同地址保留更长名字
  {
    std::vector<ElfSym> uniq;
    uniq.reserve(g_syms.size());
    for (auto& s : g_syms) {
      if (!uniq.empty() && uniq.back().addr == s.addr) {
        if (s.name.size() > uniq.back().name.size()) uniq.back() = s;
        continue;
      }
      uniq.push_back(std::move(s));
    }
    g_syms.swap(uniq);
  }
  for (size_t i = 0; i < g_syms.size(); ++i) g_exact[g_syms[i].addr] = i;
  g_loaded = true;
  return (int)g_syms.size();
}

size_t sym_count() { return g_syms.size(); }

const char* sym_name_at(uintptr_t addr) {
  if (!g_loaded && is_attached()) sym_refresh();
  auto it = g_exact.find(addr);
  if (it != g_exact.end()) return g_syms[it->second].name.c_str();
  // 落在函数范围内
  if (g_syms.empty()) return "";
  auto lb = std::upper_bound(
      g_syms.begin(), g_syms.end(), addr,
      [](uintptr_t a, const ElfSym& s) { return a < s.addr; });
  if (lb == g_syms.begin()) return "";
  --lb;
  if (lb->size > 0 && addr >= lb->addr && addr < lb->addr + lb->size)
    return lb->name.c_str();
  // 无 size 时：与下一符号之间算本函数（仅精确入口显示）
  if (lb->addr == addr) return lb->name.c_str();
  return "";
}

bool sym_lookup(uintptr_t addr, SymInfo& out) {
  if (!g_loaded && is_attached()) sym_refresh();
  out = {};
  auto it = g_exact.find(addr);
  size_t idx = (size_t)-1;
  if (it != g_exact.end()) {
    idx = it->second;
  } else if (!g_syms.empty()) {
    auto lb = std::upper_bound(
        g_syms.begin(), g_syms.end(), addr,
        [](uintptr_t a, const ElfSym& s) { return a < s.addr; });
    if (lb != g_syms.begin()) {
      --lb;
      if (lb->size > 0 && addr >= lb->addr && addr < lb->addr + lb->size)
        idx = (size_t)(lb - g_syms.begin());
      else if (lb->addr == addr)
        idx = (size_t)(lb - g_syms.begin());
    }
  }
  if (idx == (size_t)-1) return false;
  auto& s = g_syms[idx];
  out.addr = s.addr;
  out.size = s.size;
  std::snprintf(out.name, sizeof(out.name), "%s", s.name.c_str());
  std::snprintf(out.module, sizeof(out.module), "%s", s.module.c_str());
  return true;
}

bool sym_nearest(uintptr_t addr, SymInfo& out, size_t max_dist) {
  if (!g_loaded && is_attached()) sym_refresh();
  out = {};
  if (g_syms.empty()) return false;
  auto lb = std::upper_bound(
      g_syms.begin(), g_syms.end(), addr,
      [](uintptr_t a, const ElfSym& s) { return a < s.addr; });
  const ElfSym* best = nullptr;
  size_t best_d = max_dist + 1;
  if (lb != g_syms.end()) {
    size_t d = lb->addr >= addr ? lb->addr - addr : addr - lb->addr;
    if (d <= max_dist) {
      best = &*lb;
      best_d = d;
    }
  }
  if (lb != g_syms.begin()) {
    auto p = lb - 1;
    size_t d = addr >= p->addr ? addr - p->addr : p->addr - addr;
    if (d < best_d && d <= max_dist) {
      best = &*p;
      best_d = d;
    }
  }
  if (!best) return false;
  out.addr = best->addr;
  out.size = best->size;
  std::snprintf(out.name, sizeof(out.name), "%s", best->name.c_str());
  std::snprintf(out.module, sizeof(out.module), "%s", best->module.c_str());
  return true;
}

bool sym_find_by_name(const char* name, uintptr_t& out_addr, char* mod_out,
                      size_t mod_cap) {
  out_addr = 0;
  if (!name || !name[0]) return false;
  if (!g_loaded && is_attached()) sym_refresh();
  if (g_syms.empty()) return false;

  auto score_mod = [](const std::string& m) -> int {
    // 越高越优先
    if (m.find("libdl") != std::string::npos) return 100;
    if (m.find("linker64") != std::string::npos) return 95;
    if (m.find("linker") != std::string::npos) return 90;
    if (m.find("libc.so") != std::string::npos) return 80;
    if (m.find("bionic") != std::string::npos) return 70;
    return 10;
  };

  const ElfSym* best = nullptr;
  int best_score = -1;
  size_t nlen = std::strlen(name);
  for (auto& s : g_syms) {
    if (s.name == name ||
        (s.name.size() >= nlen &&
         s.name.compare(s.name.size() - nlen, nlen, name) == 0 &&
         (s.name.size() == nlen ||
          s.name[s.name.size() - nlen - 1] == '_' ||
          s.name[s.name.size() - nlen - 1] == '.'))) {
      int sc = score_mod(s.module);
      if (s.name == name) sc += 50;
      // Android: __loader_dlopen 更准
      if (s.name.find("__loader_") == 0) sc += 20;
      if (sc > best_score) {
        best_score = sc;
        best = &s;
      }
    }
  }
  // 再扫包含名
  if (!best) {
    for (auto& s : g_syms) {
      if (s.name.find(name) != std::string::npos) {
        int sc = score_mod(s.module);
        if (sc > best_score) {
          best_score = sc;
          best = &s;
        }
      }
    }
  }
  if (!best || best->addr == 0) return false;
  out_addr = best->addr;
  if (mod_out && mod_cap)
    std::snprintf(mod_out, mod_cap, "%s", best->module.c_str());
  return true;
}

}  // namespace mem
