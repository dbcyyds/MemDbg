/**
 * ARM64 轻量汇编器：常用指令 → 机器码，写回内存
 */
#include "mem_disasm.hpp"
#include "mem_core.hpp"
#include "mem_bp.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace mem {
namespace {

void set_err(char* err, size_t cap, const char* msg) {
  if (err && cap) std::snprintf(err, cap, "%s", msg ? msg : "error");
}

void skip_ws(const char*& p) {
  while (*p && std::isspace((unsigned char)*p)) ++p;
}

bool parse_reg(const char*& p, int& reg, bool& is64, bool& is_sp) {
  skip_ws(p);
  is_sp = false;
  is64 = true;
  if (!p[0]) return false;
  if ((p[0] == 's' || p[0] == 'S') && (p[1] == 'p' || p[1] == 'P') &&
      !std::isalnum((unsigned char)p[2])) {
    reg = 31;
    is_sp = true;
    is64 = true;
    p += 2;
    return true;
  }
  if ((p[0] == 'l' || p[0] == 'L') && (p[1] == 'r' || p[1] == 'R') &&
      !std::isalnum((unsigned char)p[2])) {
    reg = 30;
    is64 = true;
    p += 2;
    return true;
  }
  if ((p[0] == 'f' || p[0] == 'F') && (p[1] == 'p' || p[1] == 'P') &&
      !std::isalnum((unsigned char)p[2])) {
    reg = 29;
    is64 = true;
    p += 2;
    return true;
  }
  if ((p[0] == 'w' || p[0] == 'W' || p[0] == 'x' || p[0] == 'X') &&
      std::isdigit((unsigned char)p[1])) {
    is64 = (p[0] == 'x' || p[0] == 'X');
    p++;
    char* end = nullptr;
    long v = std::strtol(p, &end, 10);
    if (end == p || v < 0 || v > 31) return false;
    reg = (int)v;
    p = end;
    return true;
  }
  if ((p[0] == 'w' || p[0] == 'W') && (p[1] == 'z' || p[1] == 'Z') &&
      (p[2] == 'r' || p[2] == 'R')) {
    reg = 31;
    is64 = false;
    p += 3;
    return true;
  }
  if ((p[0] == 'x' || p[0] == 'X') && (p[1] == 'z' || p[1] == 'Z') &&
      (p[2] == 'r' || p[2] == 'R')) {
    reg = 31;
    is64 = true;
    p += 3;
    return true;
  }
  return false;
}

bool expect_char(const char*& p, char c) {
  skip_ws(p);
  if (*p != c) return false;
  ++p;
  return true;
}

bool parse_imm(const char*& p, int64_t& imm) {
  skip_ws(p);
  if (*p == '#') ++p;
  skip_ws(p);
  int base = 0;
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) base = 16;
  char* end = nullptr;
  imm = (int64_t)std::strtoll(p, &end, base);
  if (end == p) return false;
  p = end;
  return true;
}

bool parse_addr_imm(const char*& p, uint64_t& addr) {
  skip_ws(p);
  if (*p == '#') ++p;
  skip_ws(p);
  int base = 0;
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) base = 16;
  char* end = nullptr;
  addr = (uint64_t)std::strtoull(p, &end, base);
  if (end == p) return false;
  p = end;
  return true;
}

std::string lower_token(const char* s, size_t n) {
  std::string o;
  o.reserve(n);
  for (size_t i = 0; i < n; ++i) o.push_back((char)std::tolower((unsigned char)s[i]));
  return o;
}

// 编码 helpers
uint32_t enc_nop() { return 0xD503201F; }
uint32_t enc_ret() { return 0xD65F03C0; }
uint32_t enc_brk(int imm) {
  return 0xD4200000u | ((uint32_t)(imm & 0xFFFF) << 5);
}
uint32_t enc_svc(int imm) {
  return 0xD4000001u | ((uint32_t)(imm & 0xFFFF) << 5);
}

uint32_t enc_br(int rn, bool link) {
  // BR / BLR
  return (link ? 0xD63F0000u : 0xD61F0000u) | ((uint32_t)(rn & 0x1F) << 5);
}

uint32_t enc_b_imm(int64_t byte_off, bool link) {
  int64_t imm26 = byte_off >> 2;
  return (link ? 0x94000000u : 0x14000000u) | ((uint32_t)imm26 & 0x3FFFFFF);
}

uint32_t enc_b_cond(int64_t byte_off, int cond) {
  int64_t imm19 = byte_off >> 2;
  return 0x54000000u | ((uint32_t)(imm19 & 0x7FFFF) << 5) | (cond & 0xF);
}

uint32_t enc_cbz(int rt, bool is64, bool nz, int64_t byte_off) {
  int64_t imm19 = byte_off >> 2;
  uint32_t base = nz ? 0x35000000u : 0x34000000u;
  if (is64) base |= (1u << 31);
  return base | ((uint32_t)(imm19 & 0x7FFFF) << 5) | (rt & 0x1F);
}

uint32_t enc_movz(int rd, bool is64, int imm16, int hw) {
  uint32_t w = 0x52800000u | ((uint32_t)(hw & 3) << 21) |
               ((uint32_t)(imm16 & 0xFFFF) << 5) | (rd & 0x1F);
  if (is64) w |= (1u << 31);
  return w;
}
uint32_t enc_movk(int rd, bool is64, int imm16, int hw) {
  uint32_t w = 0x72800000u | ((uint32_t)(hw & 3) << 21) |
               ((uint32_t)(imm16 & 0xFFFF) << 5) | (rd & 0x1F);
  if (is64) w |= (1u << 31);
  return w;
}
uint32_t enc_movn(int rd, bool is64, int imm16, int hw) {
  uint32_t w = 0x12800000u | ((uint32_t)(hw & 3) << 21) |
               ((uint32_t)(imm16 & 0xFFFF) << 5) | (rd & 0x1F);
  if (is64) w |= (1u << 31);
  return w;
}

// ORR Rd, XZR, Rm → MOV
uint32_t enc_mov_reg(int rd, int rm, bool is64) {
  uint32_t w = 0x2A0003E0u | ((uint32_t)(rm & 0x1F) << 16) | (rd & 0x1F);
  if (is64) w |= (1u << 31);
  return w;
}

uint32_t enc_add_sub_imm(int rd, int rn, int imm, bool is64, bool sub) {
  uint32_t sh = 0;
  if (imm < 0) return 0;
  if (imm > 0xFFF) {
    if ((imm & 0xFFF) == 0 && (imm >> 12) <= 0xFFF) {
      imm >>= 12;
      sh = 1;
    } else
      return 0;
  }
  uint32_t w = (sub ? 0x51000000u : 0x11000000u) | (sh << 22) |
               ((uint32_t)(imm & 0xFFF) << 10) | ((uint32_t)(rn & 0x1F) << 5) |
               (rd & 0x1F);
  if (is64) w |= (1u << 31);
  return w;
}

uint32_t enc_add_sub_reg(int rd, int rn, int rm, bool is64, bool sub) {
  uint32_t w = (sub ? 0x4B000000u : 0x0B000000u) |
               ((uint32_t)(rm & 0x1F) << 16) | ((uint32_t)(rn & 0x1F) << 5) |
               (rd & 0x1F);
  if (is64) w |= (1u << 31);
  // shifted register form has bit 21 = 0; 0x0B000000 already ok for 32-bit
  // for 64-bit SUB is 0xCB000000
  if (is64 && sub) w = 0xCB000000u | ((uint32_t)(rm & 0x1F) << 16) |
                       ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
  if (is64 && !sub)
    w = 0x8B000000u | ((uint32_t)(rm & 0x1F) << 16) |
        ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
  return w;
}

uint32_t enc_subs_imm(int rd, int rn, int imm, bool is64) {
  uint32_t sh = 0;
  if (imm > 0xFFF) {
    if ((imm & 0xFFF) == 0 && (imm >> 12) <= 0xFFF) {
      imm >>= 12;
      sh = 1;
    } else
      return 0;
  }
  uint32_t w = 0x71000000u | (sh << 22) | ((uint32_t)(imm & 0xFFF) << 10) |
               ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
  if (is64) w |= (1u << 31);
  return w;
}

uint32_t enc_ldr_str_imm(int rt, int rn, int imm, bool is64, bool load,
                         int size_log2) {
  // unsigned offset
  int scale = size_log2;
  if (imm < 0 || (imm & ((1 << scale) - 1)) != 0) return 0;
  int imm12 = imm >> scale;
  if (imm12 > 0xFFF) return 0;
  uint32_t size = (uint32_t)size_log2 << 30;
  uint32_t opc = load ? (1u << 22) : 0;
  return 0x39000000u | size | opc | ((uint32_t)(imm12 & 0xFFF) << 10) |
         ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
}

int cond_code(const char* c) {
  static const char* names[] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                                "hi", "ls", "ge", "lt", "gt", "le", "al", "nv",
                                "hs", "lo"};
  // hs=cs, lo=cc
  for (int i = 0; i < 16; ++i) {
    if (std::strcmp(c, names[i]) == 0) return i;
  }
  if (std::strcmp(c, "hs") == 0) return 2;
  if (std::strcmp(c, "lo") == 0) return 3;
  return -1;
}

bool assemble_one(const char* line, uintptr_t pc, uint32_t& out, char* err,
                  size_t err_cap) {
  out = 0;
  if (!line) {
    set_err(err, err_cap, "empty");
    return false;
  }
  // strip comment
  char buf[256];
  std::snprintf(buf, sizeof(buf), "%s", line);
  for (char* q = buf; *q; ++q) {
    if (*q == ';' || *q == '/') {
      if (*q == '/' && q[1] != '/') continue;
      *q = 0;
      break;
    }
  }
  const char* p = buf;
  skip_ws(p);
  if (!*p) {
    set_err(err, err_cap, "empty line");
    return false;
  }

  // mnemonic
  const char* m0 = p;
  while (*p && !std::isspace((unsigned char)*p) && *p != '.') ++p;
  // b.eq form
  std::string mnem;
  if (*p == '.' || (m0 < p)) {
    // handle b.cond as full token including dot
  }
  p = m0;
  while (*p && !std::isspace((unsigned char)*p)) ++p;
  mnem = lower_token(m0, (size_t)(p - m0));
  skip_ws(p);

  if (mnem == "nop") {
    out = enc_nop();
    return true;
  }
  if (mnem == "ret") {
    out = enc_ret();
    return true;
  }
  if (mnem == "brk") {
    int64_t imm = 0;
    if (*p && !parse_imm(p, imm)) {
      set_err(err, err_cap, "brk imm");
      return false;
    }
    out = enc_brk((int)imm);
    return true;
  }
  if (mnem == "svc") {
    int64_t imm = 0;
    if (*p && !parse_imm(p, imm)) {
      set_err(err, err_cap, "svc imm");
      return false;
    }
    out = enc_svc((int)imm);
    return true;
  }
  if (mnem == "br" || mnem == "blr") {
    int rn;
    bool is64, sp;
    if (!parse_reg(p, rn, is64, sp)) {
      set_err(err, err_cap, "br reg");
      return false;
    }
    out = enc_br(rn, mnem == "blr");
    return true;
  }

  // b / bl / b.cond
  if (mnem == "b" || mnem == "bl" ||
      (mnem.size() > 2 && mnem[0] == 'b' && mnem[1] == '.')) {
    uint64_t target = 0;
    bool has = parse_addr_imm(p, target);
    if (!has) {
      set_err(err, err_cap, "b/bl need absolute addr 0x...");
      return false;
    }
    int64_t off = (int64_t)target - (int64_t)pc;
    if (mnem == "b") {
      out = enc_b_imm(off, false);
      return true;
    }
    if (mnem == "bl") {
      out = enc_b_imm(off, true);
      return true;
    }
    int cc = cond_code(mnem.c_str() + 2);
    if (cc < 0) {
      set_err(err, err_cap, "bad cond");
      return false;
    }
    out = enc_b_cond(off, cc);
    return true;
  }

  if (mnem == "cbz" || mnem == "cbnz") {
    int rt;
    bool is64, sp;
    if (!parse_reg(p, rt, is64, sp) || !expect_char(p, ',')) {
      set_err(err, err_cap, "cbz syntax");
      return false;
    }
    uint64_t target = 0;
    if (!parse_addr_imm(p, target)) {
      set_err(err, err_cap, "cbz target");
      return false;
    }
    int64_t off = (int64_t)target - (int64_t)pc;
    out = enc_cbz(rt, is64, mnem == "cbnz", off);
    return true;
  }

  if (mnem == "mov" || mnem == "movz" || mnem == "movk" || mnem == "movn") {
    int rd;
    bool is64, sp;
    if (!parse_reg(p, rd, is64, sp) || !expect_char(p, ',')) {
      set_err(err, err_cap, "mov rd");
      return false;
    }
    skip_ws(p);
    // mov Rd, Rm
    int rm;
    bool is64b, spb;
    const char* save = p;
    if (parse_reg(p, rm, is64b, spb) && mnem == "mov") {
      out = enc_mov_reg(rd, rm, is64);
      return true;
    }
    p = save;
    int64_t imm = 0;
    if (!parse_imm(p, imm)) {
      set_err(err, err_cap, "mov imm/reg");
      return false;
    }
    int hw = 0;
    skip_ws(p);
    if (*p == ',') {
      ++p;
      skip_ws(p);
      // lsl #N
      if ((p[0] == 'l' || p[0] == 'L') && (p[1] == 's' || p[1] == 'S')) {
        p += 3;
        skip_ws(p);
        if (*p == '#') ++p;
        int64_t sh = 0;
        parse_imm(p, sh);
        if (sh == 16) hw = 1;
        else if (sh == 32) hw = 2;
        else if (sh == 48) hw = 3;
        else if (sh != 0) {
          set_err(err, err_cap, "mov lsl must 0/16/32/48");
          return false;
        }
      }
    }
    if (imm < 0 || imm > 0xFFFF) {
      // mov Rd, #imm → movz low
      if (mnem == "mov" && imm >= 0 && imm <= 0xFFFF) {
        /* ok */
      } else if (mnem == "mov" && (uint64_t)imm <= 0xFFFFFFFFULL) {
        // only low 16 supported in single insn; use movz of low
        imm &= 0xFFFF;
      } else if (imm < 0 || imm > 0xFFFF) {
        set_err(err, err_cap, "imm16 only (use movz/movk)");
        return false;
      }
    }
    if (mnem == "movk")
      out = enc_movk(rd, is64, (int)imm, hw);
    else if (mnem == "movn")
      out = enc_movn(rd, is64, (int)imm, hw);
    else
      out = enc_movz(rd, is64, (int)imm, hw);
    return true;
  }

  if (mnem == "add" || mnem == "sub") {
    int rd, rn;
    bool is64, sp;
    if (!parse_reg(p, rd, is64, sp) || !expect_char(p, ',') ||
        !parse_reg(p, rn, is64, sp) || !expect_char(p, ',')) {
      set_err(err, err_cap, "add/sub syntax");
      return false;
    }
    skip_ws(p);
    int rm;
    bool is64b, spb;
    const char* save = p;
    if (parse_reg(p, rm, is64b, spb)) {
      out = enc_add_sub_reg(rd, rn, rm, is64, mnem == "sub");
      return true;
    }
    p = save;
    int64_t imm = 0;
    if (!parse_imm(p, imm) || imm < 0) {
      set_err(err, err_cap, "add/sub imm");
      return false;
    }
    out = enc_add_sub_imm(rd, rn, (int)imm, is64, mnem == "sub");
    if (!out) {
      set_err(err, err_cap, "imm out of range");
      return false;
    }
    return true;
  }

  if (mnem == "cmp") {
    int rn;
    bool is64, sp;
    if (!parse_reg(p, rn, is64, sp) || !expect_char(p, ',')) {
      set_err(err, err_cap, "cmp syntax");
      return false;
    }
    int64_t imm = 0;
    int rm;
    bool is64b, spb;
    const char* save = p;
    if (parse_reg(p, rm, is64b, spb)) {
      // SUBS XZR, Rn, Rm
      out = enc_add_sub_reg(31, rn, rm, is64, true);
      // need S bit: for shifted-register SUBS
      if (is64)
        out = 0xEB00001Fu | ((uint32_t)(rm & 0x1F) << 16) |
              ((uint32_t)(rn & 0x1F) << 5);
      else
        out = 0x6B00001Fu | ((uint32_t)(rm & 0x1F) << 16) |
              ((uint32_t)(rn & 0x1F) << 5);
      return true;
    }
    p = save;
    if (!parse_imm(p, imm) || imm < 0) {
      set_err(err, err_cap, "cmp imm");
      return false;
    }
    out = enc_subs_imm(31, rn, (int)imm, is64);
    return true;
  }

  if (mnem == "ldr" || mnem == "str" || mnem == "ldrb" || mnem == "strb" ||
      mnem == "ldrh" || mnem == "strh") {
    int rt, rn;
    bool is64, sp;
    if (!parse_reg(p, rt, is64, sp) || !expect_char(p, ',')) {
      set_err(err, err_cap, "ldr/str rt");
      return false;
    }
    skip_ws(p);
    if (!expect_char(p, '[')) {
      set_err(err, err_cap, "expect [");
      return false;
    }
    if (!parse_reg(p, rn, is64, sp)) {
      set_err(err, err_cap, "ldr/str rn");
      return false;
    }
    int64_t imm = 0;
    skip_ws(p);
    if (*p == ',') {
      ++p;
      if (!parse_imm(p, imm)) {
        set_err(err, err_cap, "ldr imm");
        return false;
      }
    }
    if (!expect_char(p, ']')) {
      set_err(err, err_cap, "expect ]");
      return false;
    }
    bool load = mnem[0] == 'l';
    int scale = 3;
    if (mnem == "ldrb" || mnem == "strb") {
      scale = 0;
      is64 = false;
    } else if (mnem == "ldrh" || mnem == "strh") {
      scale = 1;
      is64 = false;
    } else if (!is64)
      scale = 2;
    else
      scale = 3;
    out = enc_ldr_str_imm(rt, rn, (int)imm, is64, load, scale);
    if (!out && imm == 0) {
      // zero offset still valid — enc returns non-zero usually
      out = enc_ldr_str_imm(rt, rn, 0, is64, load, scale);
    }
    if (imm == 0) {
      // force encode even if function returned 0 incorrectly
      uint32_t size = (uint32_t)scale << 30;
      uint32_t opc = load ? (1u << 22) : 0;
      out = 0x39000000u | size | opc | ((uint32_t)(rn & 0x1F) << 5) |
            (rt & 0x1F);
    }
    if (!out) {
      set_err(err, err_cap, "ldr/str encode fail");
      return false;
    }
    return true;
  }

  // 原始字：.word 0x........
  if (mnem == ".word" || mnem == "word" || mnem == "dcd") {
    int64_t imm = 0;
    if (!parse_imm(p, imm)) {
      set_err(err, err_cap, ".word imm");
      return false;
    }
    out = (uint32_t)imm;
    return true;
  }

  set_err(err, err_cap, "unsupported mnemonic");
  return false;
}

}  // namespace

bool assemble_line(const char* line, uint32_t& out_word, char* err,
                   size_t err_cap) {
  return assemble_one(line, 0, out_word, err, err_cap);
}

bool assemble_text(const char* text, std::vector<uint32_t>& out, char* err,
                   size_t err_cap) {
  out.clear();
  if (!text) {
    set_err(err, err_cap, "null");
    return false;
  }
  std::string s(text);
  // 统一分隔
  for (char& c : s) {
    if (c == ';') c = '\n';
  }
  uintptr_t pc = 0;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find('\n', i);
    if (j == std::string::npos) j = s.size();
    std::string line = s.substr(i, j - i);
    i = j + 1;
    // trim
    while (!line.empty() && std::isspace((unsigned char)line.back()))
      line.pop_back();
    size_t a = 0;
    while (a < line.size() && std::isspace((unsigned char)line[a])) ++a;
    if (a >= line.size()) continue;
    line = line.substr(a);
    uint32_t w = 0;
    char ebuf[96];
    if (!assemble_one(line.c_str(), pc, w, ebuf, sizeof(ebuf))) {
      set_err(err, err_cap, ebuf);
      return false;
    }
    out.push_back(w);
    pc += 4;
  }
  if (out.empty()) {
    set_err(err, err_cap, "no instructions");
    return false;
  }
  return true;
}

bool patch_asm(uintptr_t addr, const char* asm_text) {
  std::vector<uint32_t> words;
  char err[128]{};
  if (!asm_text) return false;
  std::string s(asm_text);
  for (char& c : s)
    if (c == ';') c = '\n';
  uintptr_t pc = addr & ~3ull;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find('\n', i);
    if (j == std::string::npos) j = s.size();
    std::string line = s.substr(i, j - i);
    i = j + 1;
    while (!line.empty() && std::isspace((unsigned char)line.back()))
      line.pop_back();
    size_t a = 0;
    while (a < line.size() && std::isspace((unsigned char)line[a])) ++a;
    if (a >= line.size()) continue;
    line = line.substr(a);
    uint32_t w = 0;
    if (!assemble_one(line.c_str(), pc, w, err, sizeof(err))) return false;
    words.push_back(w);
    pc += 4;
  }
  if (words.empty()) return false;
  // 走统一补丁路径（状态写 bp_status）
  return patch_bytes(addr & ~3ull, reinterpret_cast<const uint8_t*>(words.data()),
                     words.size() * 4);
}

bool assemble_to_hex(const char* line, char* hex_out, size_t hex_cap) {
  uint32_t w = 0;
  char err[64];
  if (!assemble_line(line, w, err, sizeof(err))) return false;
  if (!hex_out || hex_cap < 12) return false;
  std::snprintf(hex_out, hex_cap, "%02X %02X %02X %02X", w & 0xFF,
                (w >> 8) & 0xFF, (w >> 16) & 0xFF, (w >> 24) & 0xFF);
  return true;
}

}  // namespace mem
