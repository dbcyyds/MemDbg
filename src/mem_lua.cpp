/**
 * 内嵌 Lua 5.4 — 绑定 MemDbg 全部工具 API
 */
#include "mem_lua.hpp"
#include "mem_core.hpp"
#include "mem_bp.hpp"
#include "mem_game.hpp"
#include "mem_disasm.hpp"
#include "mem_ptrscan.hpp"
#include "mem_struct.hpp"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace mem {
namespace {

char g_log[12288]{};
uintptr_t g_last_aob = 0;
lua_State* g_L = nullptr;

void log_append(const char* s) {
  if (!s) return;
  size_t n = std::strlen(g_log);
  size_t m = std::strlen(s);
  if (n + m + 2 >= sizeof(g_log)) {
    std::memmove(g_log, g_log + sizeof(g_log) / 2, sizeof(g_log) / 2);
    n = std::strlen(g_log);
  }
  std::snprintf(g_log + n, sizeof(g_log) - n, "%s\n", s);
}

static int l_print(lua_State* L) {
  int n = lua_gettop(L);
  std::string line;
  for (int i = 1; i <= n; ++i) {
    if (i > 1) line += ' ';
    size_t len = 0;
    const char* s = luaL_tolstring(L, i, &len);
    if (s) line.append(s, len);
    lua_pop(L, 1);
  }
  log_append(line.c_str());
  return 0;
}

static uintptr_t check_addr(lua_State* L, int idx) {
  if (lua_type(L, idx) == LUA_TSTRING) {
    const char* s = lua_tostring(L, idx);
    uintptr_t a = 0;
    if (s && (!std::strcmp(s, "$aob") || !std::strcmp(s, "aob"))) {
      if (!g_last_aob) luaL_error(L, "no aob result");
      return g_last_aob;
    }
    if (!parse_addr(s, a)) luaL_error(L, "bad address: %s", s ? s : "?");
    return a;
  }
  return (uintptr_t)(uint64_t)luaL_checkinteger(L, idx);
}

static void push_hex(lua_State* L, uintptr_t a) {
  char b[32];
  std::snprintf(b, sizeof(b), "0x%llX", (unsigned long long)a);
  lua_pushstring(L, b);
}

// ── process ──────────────────────────────────────────────
static int l_is_attached(lua_State* L) {
  lua_pushboolean(L, is_attached());
  return 1;
}
static int l_pid(lua_State* L) {
  lua_pushinteger(L, attached_pid());
  return 1;
}
static int l_name(lua_State* L) {
  lua_pushstring(L, attached_name());
  return 1;
}
static int l_attach(lua_State* L) {
  int p = (int)luaL_checkinteger(L, 1);
  lua_pushboolean(L, attach(p));
  return 1;
}
static int l_detach(lua_State* L) {
  (void)L;
  detach();
  return 0;
}
static int l_list_procs(lua_State* L) {
  const char* filter = luaL_optstring(L, 1, nullptr);
  ProcListOptions o;
  o.filter = filter;
  o.skip_no_icon = false;
  o.fuzzy_filter = true;
  auto v = list_processes(o);
  lua_newtable(L);
  int i = 1;
  for (auto& p : v) {
    lua_newtable(L);
    lua_pushinteger(L, p.pid);
    lua_setfield(L, -2, "pid");
    lua_pushstring(L, p.name.c_str());
    lua_setfield(L, -2, "name");
    lua_pushstring(L, p.package.c_str());
    lua_setfield(L, -2, "package");
    lua_pushboolean(L, p.is_tencent);
    lua_setfield(L, -2, "tencent");
    lua_rawseti(L, -2, i++);
    if (i > 500) break;
  }
  return 1;
}

// ── rw helpers ───────────────────────────────────────────
#define RD(name, T, push)                                                      \
  static int l_read_##name(lua_State* L) {                                     \
    uintptr_t a = check_addr(L, 1);                                            \
    T v{};                                                                     \
    if (!read_mem(a, &v, sizeof(T))) return luaL_error(L, "read fail");        \
    push;                                                                      \
    return 1;                                                                  \
  }
#define WR(name, T, get)                                                       \
  static int l_write_##name(lua_State* L) {                                    \
    uintptr_t a = check_addr(L, 1);                                            \
    T v = get;                                                                 \
    if (!write_mem(a, &v, sizeof(T))) return luaL_error(L, "write fail");      \
    return 0;                                                                  \
  }

RD(i8, int8_t, lua_pushinteger(L, v))
RD(i16, int16_t, lua_pushinteger(L, v))
RD(i32, int32_t, lua_pushinteger(L, v))
RD(i64, int64_t, lua_pushinteger(L, (lua_Integer)v))
RD(u32, uint32_t, lua_pushinteger(L, (lua_Integer)v))
RD(u64, uint64_t, lua_pushinteger(L, (lua_Integer)v))
RD(f32, float, lua_pushnumber(L, v))
RD(f64, double, lua_pushnumber(L, v))
WR(i8, int8_t, (int8_t)luaL_checkinteger(L, 2))
WR(i16, int16_t, (int16_t)luaL_checkinteger(L, 2))
WR(i32, int32_t, (int32_t)luaL_checkinteger(L, 2))
WR(i64, int64_t, (int64_t)luaL_checkinteger(L, 2))
WR(u32, uint32_t, (uint32_t)luaL_checkinteger(L, 2))
WR(u64, uint64_t, (uint64_t)luaL_checkinteger(L, 2))
WR(f32, float, (float)luaL_checknumber(L, 2))
WR(f64, double, (double)luaL_checknumber(L, 2))

static int l_read_str(lua_State* L) {
  uintptr_t a = check_addr(L, 1);
  int maxn = (int)luaL_optinteger(L, 2, 256);
  if (maxn < 1) maxn = 1;
  if (maxn > 4096) maxn = 4096;
  std::vector<char> buf((size_t)maxn + 1, 0);
  if (!read_mem(a, buf.data(), (size_t)maxn)) return luaL_error(L, "read fail");
  lua_pushstring(L, buf.data());
  return 1;
}
static int l_read_bytes(lua_State* L) {
  uintptr_t a = check_addr(L, 1);
  int n = (int)luaL_checkinteger(L, 2);
  if (n < 1) n = 1;
  if (n > 8192) n = 8192;
  std::vector<uint8_t> buf((size_t)n);
  if (!read_mem(a, buf.data(), (size_t)n)) return luaL_error(L, "read fail");
  lua_pushlstring(L, (const char*)buf.data(), (size_t)n);
  return 1;
}
static int l_write_str(lua_State* L) {
  uintptr_t a = check_addr(L, 1);
  size_t len = 0;
  const char* s = luaL_checklstring(L, 2, &len);
  bool nul = lua_isnoneornil(L, 3) || lua_toboolean(L, 3);
  size_t n = len + (nul ? 1 : 0);
  if (n > 4096) return luaL_error(L, "string too long");
  std::vector<char> buf(n, 0);
  if (len) std::memcpy(buf.data(), s, len);
  if (!write_mem(a, buf.data(), n)) return luaL_error(L, "write fail");
  return 0;
}
static int l_write_hex(lua_State* L) {
  uintptr_t a = check_addr(L, 1);
  if (!patch_hex(a, luaL_checkstring(L, 2))) return luaL_error(L, "write_hex fail");
  return 0;
}
static int l_write_bytes(lua_State* L) {
  uintptr_t a = check_addr(L, 1);
  size_t len = 0;
  const char* s = luaL_checklstring(L, 2, &len);
  if (!write_mem(a, s, len)) return luaL_error(L, "write fail");
  return 0;
}
static int l_read_mem(lua_State* L) {  // raw alias
  return l_read_bytes(L);
}
static int l_write_mem(lua_State* L) {
  return l_write_bytes(L);
}

// ── freeze ───────────────────────────────────────────────
static ValType type_from_str(const char* t) {
  if (!t) return ValType::I32;
  if (!std::strcmp(t, "i8")) return ValType::I8;
  if (!std::strcmp(t, "i16")) return ValType::I16;
  if (!std::strcmp(t, "i32") || !std::strcmp(t, "int")) return ValType::I32;
  if (!std::strcmp(t, "i64") || !std::strcmp(t, "long")) return ValType::I64;
  if (!std::strcmp(t, "f32") || !std::strcmp(t, "float")) return ValType::F32;
  if (!std::strcmp(t, "f64") || !std::strcmp(t, "double")) return ValType::F64;
  return ValType::I32;
}
static int l_freeze(lua_State* L) {
  uintptr_t a = check_addr(L, 1);
  const char* ts = luaL_optstring(L, 2, "i32");
  ValType t = type_from_str(ts);
  size_t sz = type_size_of(t);
  uint64_t bits = 0;
  if (lua_isnumber(L, 3)) {
    if (t == ValType::F32) {
      float f = (float)lua_tonumber(L, 3);
      std::memcpy(&bits, &f, 4);
      sz = 4;
    } else if (t == ValType::F64) {
      double d = (double)lua_tonumber(L, 3);
      std::memcpy(&bits, &d, 8);
      sz = 8;
    } else
      bits = (uint64_t)lua_tointeger(L, 3);
  } else
    read_mem(a, &bits, sz > 8 ? 8 : sz);
  set_frozen(a, true, bits, sz);
  return 0;
}
static int l_unfreeze(lua_State* L) {
  set_frozen(check_addr(L, 1), false, 0, 4);
  return 0;
}
static int l_clear_frozen(lua_State* L) {
  (void)L;
  clear_all_frozen();
  return 0;
}

// ── maps / modules / threads ─────────────────────────────
static int l_maps(lua_State* L) {
  bool wonly = lua_toboolean(L, 1);
  auto v = load_maps(wonly);
  const char* filt = luaL_optstring(L, 2, nullptr);
  lua_newtable(L);
  int i = 1;
  for (auto& r : v) {
    if (filt && filt[0] && r.path.find(filt) == std::string::npos &&
        !std::strstr(r.perms, filt))
      continue;
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)(uint64_t)r.start);
    lua_setfield(L, -2, "start");
    lua_pushinteger(L, (lua_Integer)(uint64_t)r.end);
    lua_setfield(L, -2, "end");
    lua_pushinteger(L, (lua_Integer)(r.end - r.start));
    lua_setfield(L, -2, "size");
    lua_pushstring(L, r.perms);
    lua_setfield(L, -2, "perms");
    lua_pushstring(L, r.path.c_str());
    lua_setfield(L, -2, "path");
    lua_rawseti(L, -2, i++);
    if (i > 2000) break;
  }
  return 1;
}
static int l_modules(lua_State* L) {
  const char* f = luaL_optstring(L, 1, nullptr);
  auto v = list_modules(f);
  lua_newtable(L);
  int i = 1;
  for (auto& m : v) {
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)(uint64_t)m.start);
    lua_setfield(L, -2, "start");
    lua_pushinteger(L, (lua_Integer)(uint64_t)m.end);
    lua_setfield(L, -2, "end");
    lua_pushstring(L, m.perms);
    lua_setfield(L, -2, "perms");
    lua_pushstring(L, m.path.c_str());
    lua_setfield(L, -2, "path");
    lua_rawseti(L, -2, i++);
  }
  return 1;
}
static int l_module_of(lua_State* L) {
  auto s = module_of(check_addr(L, 1));
  lua_pushstring(L, s.c_str());
  return 1;
}
static int l_module_base(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  uintptr_t b = 0, e = 0;
  if (!find_module_base(name, b, e, nullptr)) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushinteger(L, (lua_Integer)(uint64_t)b);
  lua_pushinteger(L, (lua_Integer)(uint64_t)e);
  return 2;
}
static int l_threads(lua_State* L) {
  auto v = list_threads();
  lua_newtable(L);
  int i = 1;
  for (auto& t : v) {
    lua_newtable(L);
    lua_pushinteger(L, t.tid);
    lua_setfield(L, -2, "tid");
    lua_pushstring(L, t.name);
    lua_setfield(L, -2, "name");
    lua_pushstring(L, t.state);
    lua_setfield(L, -2, "state");
    lua_rawseti(L, -2, i++);
  }
  return 1;
}

// ── scan ─────────────────────────────────────────────────
static ValType parse_vtype_lua(const char* s) {
  if (!s) return ValType::I32;
  if (!std::strcmp(s, "i8")) return ValType::I8;
  if (!std::strcmp(s, "i16")) return ValType::I16;
  if (!std::strcmp(s, "i32")) return ValType::I32;
  if (!std::strcmp(s, "i64")) return ValType::I64;
  if (!std::strcmp(s, "f32") || !std::strcmp(s, "float")) return ValType::F32;
  if (!std::strcmp(s, "f64") || !std::strcmp(s, "double")) return ValType::F64;
  if (!std::strcmp(s, "hex")) return ValType::Hex;
  if (!std::strcmp(s, "utf8") || !std::strcmp(s, "str")) return ValType::StrUtf8;
  if (!std::strcmp(s, "utf16")) return ValType::StrUtf16;
  return ValType::I32;
}
static ScanMode parse_smode_lua(const char* s) {
  if (!s) return ScanMode::Exact;
  if (!std::strcmp(s, "exact")) return ScanMode::Exact;
  if (!std::strcmp(s, "greater")) return ScanMode::Greater;
  if (!std::strcmp(s, "less")) return ScanMode::Less;
  if (!std::strcmp(s, "between")) return ScanMode::Between;
  if (!std::strcmp(s, "changed")) return ScanMode::Changed;
  if (!std::strcmp(s, "unchanged")) return ScanMode::Unchanged;
  if (!std::strcmp(s, "increased")) return ScanMode::Increased;
  if (!std::strcmp(s, "decreased")) return ScanMode::Decreased;
  if (!std::strcmp(s, "unknown")) return ScanMode::Unknown;
  if (!std::strcmp(s, "fuzzy")) return ScanMode::Fuzzy;
  return ScanMode::Exact;
}
static RegionFilter parse_region_lua(const char* s) {
  if (!s) return RegionFilter::Writable;
  if (!std::strcmp(s, "writable")) return RegionFilter::Writable;
  if (!std::strcmp(s, "anon") || !std::strcmp(s, "anonymous"))
    return RegionFilter::Anonymous;
  if (!std::strcmp(s, "heap")) return RegionFilter::Heap;
  if (!std::strcmp(s, "java")) return RegionFilter::Java;
  if (!std::strcmp(s, "code")) return RegionFilter::Code;
  if (!std::strcmp(s, "all") || !std::strcmp(s, "everything"))
    return RegionFilter::Everything;
  return RegionFilter::Writable;
}

// mem.scan_first(value, type?, mode?, region?, v2?)
static int l_scan_first(lua_State* L) {
  const char* v1 = luaL_checkstring(L, 1);
  const char* ty = luaL_optstring(L, 2, "i32");
  const char* md = luaL_optstring(L, 3, "exact");
  const char* rg = luaL_optstring(L, 4, "writable");
  const char* v2 = luaL_optstring(L, 5, nullptr);
  ScanConfig cfg;
  char e[96]{};
  if (!parse_scan_values(parse_vtype_lua(ty), parse_smode_lua(md), v1, v2, cfg,
                         e, sizeof(e)))
    return luaL_error(L, "%s", e[0] ? e : "parse fail");
  cfg.region = parse_region_lua(rg);
  if (!start_first_scan(cfg))
    return luaL_error(L, "%s", scan_status());
  lua_pushstring(L, scan_status());
  return 1;
}
static int l_scan_next(lua_State* L) {
  const char* v1 = luaL_optstring(L, 1, "0");
  const char* ty = luaL_optstring(L, 2, "i32");
  const char* md = luaL_optstring(L, 3, "unchanged");
  const char* v2 = luaL_optstring(L, 4, nullptr);
  ScanConfig cfg;
  char e[96]{};
  if (!parse_scan_values(parse_vtype_lua(ty), parse_smode_lua(md), v1, v2, cfg,
                         e, sizeof(e)))
    return luaL_error(L, "%s", e[0] ? e : "parse fail");
  if (!start_next_scan(cfg)) return luaL_error(L, "%s", scan_status());
  lua_pushstring(L, scan_status());
  return 1;
}
static int l_scan_wait(lua_State* L) {
  int timeout = (int)luaL_optinteger(L, 1, 120000);
  int waited = 0;
  while (scan_busy() && waited < timeout) {
    usleep(50000);
    waited += 50;
  }
  lua_pushboolean(L, !scan_busy());
  lua_pushinteger(L, (lua_Integer)result_count());
  lua_pushstring(L, scan_status());
  return 3;
}
static int l_scan_busy(lua_State* L) {
  lua_pushboolean(L, scan_busy());
  return 1;
}
static int l_scan_count(lua_State* L) {
  lua_pushinteger(L, (lua_Integer)result_count());
  return 1;
}
static int l_scan_status(lua_State* L) {
  lua_pushstring(L, scan_status());
  return 1;
}
static int l_scan_clear(lua_State* L) {
  (void)L;
  clear_scan();
  return 0;
}
static int l_scan_results(lua_State* L) {
  int maxn = (int)luaL_optinteger(L, 1, 50);
  if (maxn < 1) maxn = 1;
  if (maxn > 500) maxn = 500;
  int off = (int)luaL_optinteger(L, 2, 0);
  std::vector<Match> ms;
  copy_results_range(ms, (size_t)off, (size_t)maxn);
  lua_newtable(L);
  for (size_t i = 0; i < ms.size(); ++i) {
    lua_newtable(L);
    push_hex(L, ms[i].addr);
    lua_setfield(L, -2, "addr");
    lua_pushinteger(L, (lua_Integer)ms[i].value_bits);
    lua_setfield(L, -2, "bits");
    lua_pushboolean(L, ms[i].frozen);
    lua_setfield(L, -2, "frozen");
    lua_rawseti(L, -2, (int)i + 1);
  }
  return 1;
}
static int l_export_results(lua_State* L) {
  const char* path = luaL_checkstring(L, 1);
  lua_pushinteger(L, export_results(path));
  return 1;
}
static int l_dump(lua_State* L) {
  uintptr_t a = check_addr(L, 1);
  size_t len = (size_t)luaL_optinteger(L, 2, 4096);
  const char* path = luaL_optstring(L, 3, "/data/local/tmp/memdbg_dump.bin");
  lua_pushinteger(L, dump_mem(a, len, path));
  return 1;
}

// ── aob ──────────────────────────────────────────────────
static bool parse_pat(const char* pat, std::vector<uint8_t>& bytes,
                      std::vector<uint8_t>& mask) {
  bytes.clear();
  mask.clear();
  if (!pat) return false;
  const char* p = pat;
  while (*p) {
    while (*p == ' ' || *p == '\t') ++p;
    if (!*p) break;
    if (*p == '?') {
      bytes.push_back(0);
      mask.push_back(0);
      p += (p[1] == '?') ? 2 : 1;
      continue;
    }
    char* end = nullptr;
    long v = std::strtol(p, &end, 16);
    if (end == p) return false;
    bytes.push_back((uint8_t)v);
    mask.push_back(0xFF);
    p = end;
  }
  return !bytes.empty();
}
static bool aob_search(const std::vector<uint8_t>& pat,
                       const std::vector<uint8_t>& mask, const char* mod,
                       uintptr_t& out) {
  out = 0;
  auto maps = load_maps(false);
  for (auto& r : maps) {
    if (!r.readable) continue;
    if (mod && mod[0] && r.path.find(mod) == std::string::npos) continue;
    size_t len = r.end - r.start;
    if (len < pat.size() || len > 80u * 1024 * 1024) continue;
    const size_t chunk = 256 * 1024;
    std::vector<uint8_t> buf(chunk + pat.size());
    for (uintptr_t base = r.start; base + pat.size() <= r.end; base += chunk) {
      size_t want = std::min(chunk + pat.size(), (size_t)(r.end - base));
      if (!read_mem(base, buf.data(), want)) continue;
      for (size_t i = 0; i + pat.size() <= want; ++i) {
        bool ok = true;
        for (size_t j = 0; j < pat.size(); ++j)
          if (mask[j] && buf[i + j] != pat[j]) {
            ok = false;
            break;
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
static int l_aob(lua_State* L) {
  const char* pat = luaL_checkstring(L, 1);
  const char* mod = luaL_optstring(L, 2, nullptr);
  std::vector<uint8_t> b, m;
  if (!parse_pat(pat, b, m)) return luaL_error(L, "bad aob");
  uintptr_t hit = 0;
  if (!aob_search(b, m, mod, hit)) {
    lua_pushnil(L);
    return 1;
  }
  g_last_aob = hit;
  lua_pushinteger(L, (lua_Integer)(uint64_t)hit);
  return 1;
}
static int l_last_aob(lua_State* L) {
  if (!g_last_aob)
    lua_pushnil(L);
  else
    lua_pushinteger(L, (lua_Integer)(uint64_t)g_last_aob);
  return 1;
}

// ── patch / asm ──────────────────────────────────────────
static int l_nop(lua_State* L) {
  if (!patch_nop(check_addr(L, 1), (int)luaL_optinteger(L, 2, 1)))
    return luaL_error(L, "nop fail");
  return 0;
}
static int l_patch_hex(lua_State* L) {
  if (!patch_hex(check_addr(L, 1), luaL_checkstring(L, 2)))
    return luaL_error(L, "patch fail");
  return 0;
}
static int l_patch_asm(lua_State* L) {
  if (!patch_asm(check_addr(L, 1), luaL_checkstring(L, 2)))
    return luaL_error(L, "asm fail: %s", bp_status());
  return 0;
}
static int l_assemble(lua_State* L) {
  uint32_t w = 0;
  char e[96]{};
  if (!assemble_line(luaL_checkstring(L, 1), w, e, sizeof(e)))
    return luaL_error(L, "%s", e[0] ? e : "assemble fail");
  lua_pushinteger(L, (lua_Integer)w);
  char hx[16];
  std::snprintf(hx, sizeof(hx), "%02X %02X %02X %02X", w & 0xFF, (w >> 8) & 0xFF,
                (w >> 16) & 0xFF, (w >> 24) & 0xFF);
  lua_pushstring(L, hx);
  return 2;
}

// ── debug / bp ───────────────────────────────────────────
static int l_bp_init(lua_State* L) {
  lua_pushboolean(L, bp_init());
  lua_pushstring(L, bp_status());
  return 2;
}
static int l_bp_set(lua_State* L) {
  // bp_set(addr, "exec"|"write"|"read"|"rw", size?)
  uintptr_t a = check_addr(L, 1);
  const char* ty = luaL_optstring(L, 2, "exec");
  int sz = (int)luaL_optinteger(L, 3, 4);
  BpType t = BpType::Exec;
  if (!std::strcmp(ty, "write") || !std::strcmp(ty, "w")) t = BpType::WatchW;
  else if (!std::strcmp(ty, "read") || !std::strcmp(ty, "r")) t = BpType::WatchR;
  else if (!std::strcmp(ty, "rw")) t = BpType::WatchRW;
  int id = bp_set(a, t, sz);
  if (id < 0) return luaL_error(L, "%s", bp_status());
  lua_pushinteger(L, id);
  lua_pushstring(L, bp_status());
  return 2;
}
static int l_bp_clear(lua_State* L) {
  if (lua_isnoneornil(L, 1))
    bp_clear_all();
  else
    bp_clear((int)luaL_checkinteger(L, 1));
  return 0;
}
static int l_bp_list(lua_State* L) {
  auto v = bp_list();
  lua_newtable(L);
  int i = 1;
  for (auto& b : v) {
    lua_newtable(L);
    lua_pushinteger(L, b.id);
    lua_setfield(L, -2, "id");
    push_hex(L, b.addr);
    lua_setfield(L, -2, "addr");
    lua_pushstring(L, bp_type_name(b.type));
    lua_setfield(L, -2, "type");
    lua_pushboolean(L, b.enabled);
    lua_setfield(L, -2, "enabled");
    lua_rawseti(L, -2, i++);
  }
  return 1;
}
static int l_soft_bp(lua_State* L) {
  uintptr_t a = check_addr(L, 1);
  const char* cond = luaL_optstring(L, 2, nullptr);
  bool oneshot = lua_toboolean(L, 3);
  int id = soft_bp_set_cond(a, cond, oneshot);
  if (id < 0) return luaL_error(L, "%s", bp_status());
  lua_pushinteger(L, id);
  return 1;
}
static int l_soft_bp_clear(lua_State* L) {
  if (lua_isnoneornil(L, 1))
    soft_bp_clear_all();
  else
    soft_bp_clear((int)luaL_checkinteger(L, 1));
  return 0;
}
static int l_dbg_pause(lua_State* L) {
  lua_pushboolean(L, dbg_pause());
  return 1;
}
static int l_dbg_resume(lua_State* L) {
  lua_pushboolean(L, dbg_resume());
  return 1;
}
static int l_dbg_step(lua_State* L) {
  lua_pushboolean(L, dbg_step());
  return 1;
}
static int l_dbg_step_over(lua_State* L) {
  lua_pushboolean(L, dbg_step_over());
  return 1;
}
static int l_dbg_step_out(lua_State* L) {
  lua_pushboolean(L, dbg_step_out());
  return 1;
}
static int l_dbg_paused(lua_State* L) {
  lua_pushboolean(L, dbg_is_paused());
  return 1;
}
static int l_regs(lua_State* L) {
  Regs r{};
  if (!dbg_regs_read(r) || !r.valid) return luaL_error(L, "%s", bp_status());
  lua_newtable(L);
  for (int i = 0; i < 31; ++i) {
    char k[8];
    std::snprintf(k, sizeof(k), "x%d", i);
    lua_pushinteger(L, (lua_Integer)r.x[i]);
    lua_setfield(L, -2, k);
  }
  lua_pushinteger(L, (lua_Integer)r.sp);
  lua_setfield(L, -2, "sp");
  lua_pushinteger(L, (lua_Integer)r.pc);
  lua_setfield(L, -2, "pc");
  lua_pushinteger(L, (lua_Integer)r.x[30]);
  lua_setfield(L, -2, "lr");
  return 1;
}
static int l_reg_set(lua_State* L) {
  if (!dbg_reg_set(luaL_checkstring(L, 1), (uint64_t)luaL_checkinteger(L, 2)))
    return luaL_error(L, "%s", bp_status());
  return 0;
}
static int l_stack(lua_State* L) {
  auto v = dbg_stack_trace((int)luaL_optinteger(L, 1, 16));
  lua_newtable(L);
  for (size_t i = 0; i < v.size(); ++i) {
    push_hex(L, v[i]);
    lua_rawseti(L, -2, (int)i + 1);
  }
  return 1;
}
static int l_trace(lua_State* L) {
  std::vector<TraceEntry> tr;
  if (!dbg_trace((int)luaL_optinteger(L, 1, 16), tr))
    return luaL_error(L, "%s", bp_status());
  lua_newtable(L);
  for (size_t i = 0; i < tr.size(); ++i) {
    lua_newtable(L);
    push_hex(L, tr[i].pc);
    lua_setfield(L, -2, "pc");
    lua_pushstring(L, tr[i].text);
    lua_setfield(L, -2, "text");
    lua_rawseti(L, -2, (int)i + 1);
  }
  return 1;
}

// ── inject ───────────────────────────────────────────────
static int l_find_dlopen(lua_State* L) {
  uintptr_t a = 0;
  if (!find_dlopen(a)) return luaL_error(L, "%s", bp_status());
  lua_pushinteger(L, (lua_Integer)(uint64_t)a);
  lua_pushstring(L, bp_status());
  return 2;
}
static int l_inject_so(lua_State* L) {
  const char* path = luaL_checkstring(L, 1);
  uintptr_t dlo = 0;
  if (!lua_isnoneornil(L, 2)) dlo = check_addr(L, 2);
  uint64_t h = 0;
  if (!inject_so(dlo, path, &h)) return luaL_error(L, "%s", bp_status());
  lua_pushinteger(L, (lua_Integer)h);
  return 1;
}
static int l_remote_call(lua_State* L) {
  uintptr_t fn = check_addr(L, 1);
  uint64_t args[8]{};
  int n = 0;
  if (lua_istable(L, 2)) {
    n = (int)lua_rawlen(L, 2);
    if (n > 8) n = 8;
    for (int i = 0; i < n; ++i) {
      lua_rawgeti(L, 2, i + 1);
      args[i] = (uint64_t)lua_tointeger(L, -1);
      lua_pop(L, 1);
    }
  }
  uint64_t ret = 0;
  if (!remote_call(fn, n ? args : nullptr, n, &ret))
    return luaL_error(L, "%s", bp_status());
  lua_pushinteger(L, (lua_Integer)ret);
  return 1;
}

// ── disasm / sym ─────────────────────────────────────────
static int l_disasm(lua_State* L) {
  uintptr_t a = check_addr(L, 1);
  int n = (int)luaL_optinteger(L, 2, 16);
  DisasmOptions o;
  o.filter_noise = lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3);
  auto ins = disasm_at(a, n, o);
  lua_newtable(L);
  for (size_t i = 0; i < ins.size(); ++i) {
    lua_newtable(L);
    push_hex(L, ins[i].addr);
    lua_setfield(L, -2, "addr");
    lua_pushstring(L, ins[i].mnem);
    lua_setfield(L, -2, "mnem");
    lua_pushstring(L, ins[i].ops);
    lua_setfield(L, -2, "ops");
    lua_pushstring(L, ins[i].pseudo);
    lua_setfield(L, -2, "pseudo");
    lua_rawseti(L, -2, (int)i + 1);
  }
  return 1;
}
static int l_pseudo_c(lua_State* L) {
  uintptr_t a = check_addr(L, 1);
  int n = (int)luaL_optinteger(L, 2, 64);
  auto fn = disasm_function(a, n, {});
  auto s = insns_to_pseudo_c(fn, a);
  lua_pushstring(L, s.c_str());
  return 1;
}
static int l_sym_refresh(lua_State* L) {
  lua_pushinteger(L, sym_refresh());
  return 1;
}
static int l_sym_count(lua_State* L) {
  lua_pushinteger(L, (lua_Integer)sym_count());
  return 1;
}
static int l_sym_name(lua_State* L) {
  const char* n = sym_name_at(check_addr(L, 1));
  lua_pushstring(L, n ? n : "");
  return 1;
}
static int l_sym_find(lua_State* L) {
  uintptr_t a = 0;
  char mod[128]{};
  if (!sym_find_by_name(luaL_checkstring(L, 1), a, mod, sizeof(mod)) || !a) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushinteger(L, (lua_Integer)(uint64_t)a);
  lua_pushstring(L, mod);
  return 2;
}

// ── ptrscan ──────────────────────────────────────────────
static int l_ptrscan(lua_State* L) {
  PtrScanConfig c;
  c.target = check_addr(L, 1);
  c.max_level = (int)luaL_optinteger(L, 2, 2);
  c.max_offset = (uint32_t)luaL_optinteger(L, 3, 0x1000);
  c.static_only = lua_isnoneornil(L, 4) ? false : lua_toboolean(L, 4);
  c.max_results = (int)luaL_optinteger(L, 5, 50);
  if (!ptrscan_start(c)) return luaL_error(L, "%s", ptrscan_status());
  while (ptrscan_busy()) usleep(50000);
  lua_pushinteger(L, (lua_Integer)ptrscan_count());
  lua_pushstring(L, ptrscan_status());
  return 2;
}
static int l_ptrscan_results(lua_State* L) {
  int maxn = (int)luaL_optinteger(L, 1, 20);
  std::vector<PtrChain> ch;
  ptrscan_copy(ch, (size_t)maxn);
  lua_newtable(L);
  for (size_t i = 0; i < ch.size(); ++i) {
    lua_newtable(L);
    push_hex(L, ch[i].base);
    lua_setfield(L, -2, "base");
    lua_pushstring(L, ch[i].module.c_str());
    lua_setfield(L, -2, "module");
    lua_pushinteger(L, (lua_Integer)ch[i].base_rva);
    lua_setfield(L, -2, "rva");
    lua_newtable(L);
    for (size_t j = 0; j < ch[i].offsets.size(); ++j) {
      lua_pushinteger(L, ch[i].offsets[j]);
      lua_rawseti(L, -2, (int)j + 1);
    }
    lua_setfield(L, -2, "offsets");
    char fmt[128];
    ptrscan_format(ch[i], fmt, sizeof(fmt));
    lua_pushstring(L, fmt);
    lua_setfield(L, -2, "text");
    lua_rawseti(L, -2, (int)i + 1);
  }
  return 1;
}
static int l_ptrscan_clear(lua_State* L) {
  (void)L;
  ptrscan_clear();
  return 0;
}

// ── struct ───────────────────────────────────────────────
static int l_struct_dissect(lua_State* L) {
  Structure s;
  s.base = check_addr(L, 1);
  int n = (int)luaL_optinteger(L, 2, 16);
  s.auto_dissect(n);
  s.refresh();
  lua_newtable(L);
  for (size_t i = 0; i < s.fields.size(); ++i) {
    lua_newtable(L);
    lua_pushstring(L, s.fields[i].name);
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, s.fields[i].offset);
    lua_setfield(L, -2, "offset");
    lua_pushstring(L, field_type_name(s.fields[i].type));
    lua_setfield(L, -2, "type");
    lua_pushstring(L, s.fields[i].value);
    lua_setfield(L, -2, "value");
    if (s.fields[i].as_ptr) {
      push_hex(L, s.fields[i].as_ptr);
      lua_setfield(L, -2, "ptr");
    }
    lua_rawseti(L, -2, (int)i + 1);
  }
  return 1;
}

// ── speed / aa ───────────────────────────────────────────
static int l_speed(lua_State* L) {
  if (lua_isnoneornil(L, 1) ||
      (lua_isstring(L, 1) && !std::strcmp(lua_tostring(L, 1), "off"))) {
    speed_disable();
  } else {
    float m = (float)luaL_checknumber(L, 1);
    if (sym_count() == 0) sym_refresh();
    if (m <= 0.001f || std::fabs(m - 1.f) < 0.001f)
      speed_disable();
    else
      speed_set(m);
  }
  lua_pushstring(L, speed_status());
  return 1;
}
static int l_aa(lua_State* L) {
  const char* text = luaL_checkstring(L, 1);
  bool en = lua_isnoneornil(L, 2) ? true : lua_toboolean(L, 2);
  char e[128]{};
  if (!aa_run(text, en, e, sizeof(e)))
    return luaL_error(L, "%s", e[0] ? e : aa_status());
  lua_pushstring(L, aa_status());
  return 1;
}
static int l_aa_disable(lua_State* L) {
  (void)L;
  aa_disable_all();
  return 0;
}

static int l_sleep(lua_State* L) {
  int ms = (int)luaL_checkinteger(L, 1);
  if (ms < 0) ms = 0;
  if (ms > 60000) ms = 60000;
  usleep((useconds_t)ms * 1000);
  return 0;
}
static int l_status(lua_State* L) {
  lua_pushstring(L, bp_status());
  return 1;
}

static int l_help(lua_State* L) {
  static const char* h =
      R"(=== MemDbg Lua API ===
进程: mem.attach(pid) mem.detach() mem.is_attached() mem.pid() mem.name()
      mem.list_procs(filter?)
读写: mem.read_i8/i16/i32/i64/u32/u64/f32/f64/str/bytes(addr[,n])
      mem.write_i8/i16/i32/i64/u32/u64/f32/f64(addr,v)
      mem.write_str(addr,s) mem.write_hex(addr,"DE AD") mem.write_bytes(addr,bin)
冻结: mem.freeze(addr,"i32",v) mem.unfreeze(addr) mem.clear_frozen()
地图: mem.maps(writable?,filter?) mem.modules(filter?) mem.module_of(addr)
      mem.module_base(name) -> start,end  mem.threads()
扫描: mem.scan_first(val,type?,mode?,region?,v2?)
      mem.scan_next(val?,type?,mode?) mem.scan_wait(ms?)
      mem.scan_results(max?,offset?) mem.scan_count() mem.scan_clear()
      mem.export_results(path) mem.dump(addr,len?,path?)
AOB:  mem.aob(pat, module?) mem.last_aob()
补丁: mem.nop(addr,n?) mem.patch_hex/asm(addr,...) mem.assemble(line)
断点: mem.bp_init() mem.bp_set(addr,"exec|write|read|rw",size?)
      mem.bp_clear([id]) mem.bp_list()
      mem.soft_bp(addr,cond?,oneshot?) mem.soft_bp_clear([id])
调试: mem.pause/resume/step/step_over/step_out() mem.paused()
      mem.regs() mem.reg_set(name,v) mem.stack(n?) mem.trace(n?)
注入: mem.find_dlopen() mem.inject_so(path[,dlopen])
      mem.remote_call(fn, {args...})
反汇: mem.disasm(addr,n?) mem.pseudo_c(addr,n?)
符号: mem.sym_refresh() mem.sym_count() mem.sym_name(addr) mem.sym_find(name)
指针: mem.ptrscan(target,level?,maxoff?) mem.ptrscan_results(n?)
结构: mem.struct_dissect(base,n?)
其它: mem.speed(2.0|"off") mem.aa(text,enable?) mem.aa_disable()
      mem.sleep(ms) mem.status() print(...) mem.help()
地址可用 number 或 "$aob")";
  log_append(h);
  lua_pushstring(L, h);
  return 1;
}

static const luaL_Reg kMemLib[] = {
    {"is_attached", l_is_attached},
    {"pid", l_pid},
    {"name", l_name},
    {"attach", l_attach},
    {"detach", l_detach},
    {"list_procs", l_list_procs},
    {"read_i8", l_read_i8},
    {"read_i16", l_read_i16},
    {"read_i32", l_read_i32},
    {"read_i64", l_read_i64},
    {"read_u32", l_read_u32},
    {"read_u64", l_read_u64},
    {"read_f32", l_read_f32},
    {"read_f64", l_read_f64},
    {"read_str", l_read_str},
    {"read_bytes", l_read_bytes},
    {"read_mem", l_read_mem},
    {"write_i8", l_write_i8},
    {"write_i16", l_write_i16},
    {"write_i32", l_write_i32},
    {"write_i64", l_write_i64},
    {"write_u32", l_write_u32},
    {"write_u64", l_write_u64},
    {"write_f32", l_write_f32},
    {"write_f64", l_write_f64},
    {"write_str", l_write_str},
    {"write_hex", l_write_hex},
    {"write_bytes", l_write_bytes},
    {"write_mem", l_write_mem},
    {"freeze", l_freeze},
    {"unfreeze", l_unfreeze},
    {"clear_frozen", l_clear_frozen},
    {"maps", l_maps},
    {"modules", l_modules},
    {"module_of", l_module_of},
    {"module_base", l_module_base},
    {"threads", l_threads},
    {"scan_first", l_scan_first},
    {"scan_next", l_scan_next},
    {"scan_wait", l_scan_wait},
    {"scan_busy", l_scan_busy},
    {"scan_count", l_scan_count},
    {"scan_status", l_scan_status},
    {"scan_clear", l_scan_clear},
    {"scan_results", l_scan_results},
    {"export_results", l_export_results},
    {"dump", l_dump},
    {"aob", l_aob},
    {"last_aob", l_last_aob},
    {"nop", l_nop},
    {"patch_hex", l_patch_hex},
    {"patch_asm", l_patch_asm},
    {"assemble", l_assemble},
    {"bp_init", l_bp_init},
    {"bp_set", l_bp_set},
    {"bp_clear", l_bp_clear},
    {"bp_list", l_bp_list},
    {"soft_bp", l_soft_bp},
    {"soft_bp_clear", l_soft_bp_clear},
    {"pause", l_dbg_pause},
    {"resume", l_dbg_resume},
    {"step", l_dbg_step},
    {"step_over", l_dbg_step_over},
    {"step_out", l_dbg_step_out},
    {"paused", l_dbg_paused},
    {"regs", l_regs},
    {"reg_set", l_reg_set},
    {"stack", l_stack},
    {"trace", l_trace},
    {"find_dlopen", l_find_dlopen},
    {"inject_so", l_inject_so},
    {"remote_call", l_remote_call},
    {"disasm", l_disasm},
    {"pseudo_c", l_pseudo_c},
    {"sym_refresh", l_sym_refresh},
    {"sym_count", l_sym_count},
    {"sym_name", l_sym_name},
    {"sym_find", l_sym_find},
    {"ptrscan", l_ptrscan},
    {"ptrscan_results", l_ptrscan_results},
    {"ptrscan_clear", l_ptrscan_clear},
    {"struct_dissect", l_struct_dissect},
    {"speed", l_speed},
    {"aa", l_aa},
    {"aa_disable", l_aa_disable},
    {"sleep", l_sleep},
    {"status", l_status},
    {"help", l_help},
    {nullptr, nullptr},
};

static void open_mem_lib(lua_State* L) {
  luaL_newlib(L, kMemLib);
  lua_setglobal(L, "mem");
  // 常用全局别名
  static const char* aliases[] = {
      "read_i32", "write_i32", "read_f32", "write_f32", "write_str",
      "write_hex", "aob",      "freeze",   "unfreeze",  "speed",
      "sleep",    "help",      "pause",    "resume",    "step",
      "regs",     "disasm",    "nop",      nullptr};
  lua_getglobal(L, "mem");
  for (int i = 0; aliases[i]; ++i) {
    lua_getfield(L, -1, aliases[i]);
    lua_setglobal(L, aliases[i]);
  }
  lua_pop(L, 1);
  lua_pushcfunction(L, l_print);
  lua_setglobal(L, "print");
}

static bool ensure_lua(char* err, size_t err_cap) {
  if (g_L) return true;
  g_L = luaL_newstate();
  if (!g_L) {
    if (err && err_cap) std::snprintf(err, err_cap, "lua newstate fail");
    return false;
  }
  luaL_openlibs(g_L);
  open_mem_lib(g_L);
  return true;
}

}  // namespace

bool script_run(const char* text, char* err, size_t err_cap) {
  if (err && err_cap) err[0] = 0;
  if (!text) {
    if (err && err_cap) std::snprintf(err, err_cap, "null script");
    return false;
  }
  if (!ensure_lua(err, err_cap)) return false;
  int top = lua_gettop(g_L);
  if (luaL_loadstring(g_L, text) != LUA_OK) {
    const char* msg = lua_tostring(g_L, -1);
    if (err && err_cap)
      std::snprintf(err, err_cap, "%s", msg ? msg : "load error");
    log_append(msg ? msg : "load error");
    lua_settop(g_L, top);
    return false;
  }
  if (lua_pcall(g_L, 0, LUA_MULTRET, 0) != LUA_OK) {
    const char* msg = lua_tostring(g_L, -1);
    if (err && err_cap)
      std::snprintf(err, err_cap, "%s", msg ? msg : "runtime error");
    log_append(msg ? msg : "runtime error");
    lua_settop(g_L, top);
    return false;
  }
  lua_settop(g_L, top);
  return true;
}

bool script_run_file(const char* path, char* err, size_t err_cap) {
  if (err && err_cap) err[0] = 0;
  if (!path) {
    if (err && err_cap) std::snprintf(err, err_cap, "no path");
    return false;
  }
  if (!ensure_lua(err, err_cap)) return false;
  int top = lua_gettop(g_L);
  if (luaL_loadfile(g_L, path) != LUA_OK) {
    const char* msg = lua_tostring(g_L, -1);
    if (err && err_cap)
      std::snprintf(err, err_cap, "%s", msg ? msg : "loadfile error");
    log_append(msg ? msg : "loadfile error");
    lua_settop(g_L, top);
    return false;
  }
  if (lua_pcall(g_L, 0, LUA_MULTRET, 0) != LUA_OK) {
    const char* msg = lua_tostring(g_L, -1);
    if (err && err_cap)
      std::snprintf(err, err_cap, "%s", msg ? msg : "runtime error");
    log_append(msg ? msg : "runtime error");
    lua_settop(g_L, top);
    return false;
  }
  lua_settop(g_L, top);
  return true;
}

const char* script_log() { return g_log; }
void script_log_clear() { g_log[0] = 0; }
uintptr_t script_last_aob() { return g_last_aob; }

void script_shutdown() {
  if (g_L) {
    lua_close(g_L);
    g_L = nullptr;
  }
}

}  // namespace mem
