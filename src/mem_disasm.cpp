#include "mem_disasm.hpp"
#include "mem_core.hpp"

#include <cstdio>
#include <cstring>

namespace mem {
namespace {

const char* reg_x(int n) {
  static thread_local char bufs[8][8];
  static thread_local int ri = 0;
  char* buf = bufs[ri++ & 7];
  if (n == 31) {
    std::snprintf(buf, 8, "sp");
    return buf;
  }
  if (n == 30) {
    std::snprintf(buf, 8, "lr");
    return buf;
  }
  if (n == 29) {
    std::snprintf(buf, 8, "fp");
    return buf;
  }
  std::snprintf(buf, 8, "x%d", n);
  return buf;
}
const char* reg_w(int n) {
  static thread_local char bufs[8][8];
  static thread_local int ri = 0;
  char* buf = bufs[ri++ & 7];
  if (n == 31) {
    std::snprintf(buf, 8, "wsp");
    return buf;
  }
  std::snprintf(buf, 8, "w%d", n);
  return buf;
}
const char* reg_s(int n) {
  static thread_local char bufs[4][8];
  static thread_local int ri = 0;
  char* buf = bufs[ri++ & 3];
  std::snprintf(buf, 8, "s%d", n);
  return buf;
}
const char* reg_d(int n) {
  static thread_local char bufs[4][8];
  static thread_local int ri = 0;
  char* buf = bufs[ri++ & 3];
  std::snprintf(buf, 8, "d%d", n);
  return buf;
}

void set_insn(Insn& i, const char* m, const char* o) {
  std::snprintf(i.mnem, sizeof(i.mnem), "%s", m);
  std::snprintf(i.ops, sizeof(i.ops), "%s", o ? o : "");
  std::snprintf(i.text, sizeof(i.text), "%s %s", i.mnem, i.ops);
  i.cat = classify_mnem(m);
  i.is_noise = is_noise_mnem(m);
}

int64_t sign_extend(uint64_t v, int bits) {
  uint64_t m = 1ull << (bits - 1);
  return (int64_t)((v ^ m) - m);
}

// 解码单条 AArch64
void decode_one(Insn& out, uintptr_t addr, uint32_t w) {
  out.addr = addr;
  out.raw = w;
  out.is_branch = out.is_ret = out.is_call = out.is_cond = false;
  out.is_label = false;
  out.target = 0;
  out.xref_in = 0;
  out.mnem[0] = out.ops[0] = out.text[0] = out.pseudo[0] = 0;
  out.label[0] = out.target_name[0] = 0;

  // NOP
  if (w == 0xD503201F) {
    set_insn(out, "nop", "");
    return;
  }
  // RET
  if ((w & 0xFFFFFC1F) == 0xD65F0000) {
    set_insn(out, "ret", "");
    out.is_ret = true;
    out.is_branch = true;
    return;
  }
  // BR Xn
  if ((w & 0xFFFFFC1F) == 0xD61F0000) {
    int rn = (w >> 5) & 0x1F;
    char o[32];
    std::snprintf(o, sizeof(o), "%s", reg_x(rn));
    set_insn(out, "br", o);
    out.is_branch = true;
    return;
  }
  // BLR Xn
  if ((w & 0xFFFFFC1F) == 0xD63F0000) {
    int rn = (w >> 5) & 0x1F;
    char o[32];
    std::snprintf(o, sizeof(o), "%s", reg_x(rn));
    set_insn(out, "blr", o);
    out.is_call = true;
    out.is_branch = true;
    return;
  }
  // B imm26
  if ((w >> 26) == 0x5) {
    int64_t imm = sign_extend(w & 0x3FFFFFF, 26) << 2;
    out.target = addr + (uintptr_t)imm;
    char o[40];
    std::snprintf(o, sizeof(o), "0x%llX", (unsigned long long)out.target);
    set_insn(out, "b", o);
    out.is_branch = true;
    return;
  }
  // BL imm26
  if ((w >> 26) == 0x25) {
    int64_t imm = sign_extend(w & 0x3FFFFFF, 26) << 2;
    out.target = addr + (uintptr_t)imm;
    char o[40];
    std::snprintf(o, sizeof(o), "0x%llX", (unsigned long long)out.target);
    set_insn(out, "bl", o);
    out.is_call = true;
    out.is_branch = true;
    return;
  }
  // B.cond
  if ((w & 0xFF000010) == 0x54000000) {
    int64_t imm = sign_extend((w >> 5) & 0x7FFFF, 19) << 2;
    out.target = addr + (uintptr_t)imm;
    static const char* cc[] = {"eq","ne","cs","cc","mi","pl","vs","vc",
                               "hi","ls","ge","lt","gt","le","al","nv"};
    int c = w & 0xF;
    char m[16], o[40];
    std::snprintf(m, sizeof(m), "b.%s", cc[c]);
    std::snprintf(o, sizeof(o), "0x%llX", (unsigned long long)out.target);
    set_insn(out, m, o);
    out.is_branch = true;
    out.is_cond = true;
    return;
  }
  // CBZ / CBNZ
  if ((w & 0x7E000000) == 0x34000000 || (w & 0x7E000000) == 0x35000000) {
    bool is64 = (w >> 31) & 1;
    bool nz = (w >> 24) & 1;
    int rt = w & 0x1F;
    int64_t imm = sign_extend((w >> 5) & 0x7FFFF, 19) << 2;
    out.target = addr + (uintptr_t)imm;
    char o[48];
    std::snprintf(o, sizeof(o), "%s, 0x%llX", is64 ? reg_x(rt) : reg_w(rt),
                  (unsigned long long)out.target);
    set_insn(out, nz ? "cbnz" : "cbz", o);
    out.is_branch = true;
    out.is_cond = true;
    return;
  }
  // MOVZ / MOVN / MOVK
  if ((w & 0x1F800000) == 0x12800000 || (w & 0x1F800000) == 0x52800000 ||
      (w & 0x1F800000) == 0x72800000) {
    bool is64 = (w >> 31) & 1;
    int opc = (w >> 29) & 3;
    int hw = (w >> 21) & 3;
    int imm = (w >> 5) & 0xFFFF;
    int rd = w & 0x1F;
    const char* m =
        opc == 0 ? "movn" : (opc == 2 ? "movz" : (opc == 3 ? "movk" : "mov?"));
    char o[48];
    std::snprintf(o, sizeof(o), "%s, #0x%X, lsl #%d", is64 ? reg_x(rd) : reg_w(rd),
                  imm, hw * 16);
    set_insn(out, m, o);
    return;
  }
  // Logical shifted register: AND/BIC/ORR/ORN/EOR/EON/ANDS (+ MOV alias)
  // opc in bits 29-30, N in bit 21; pattern bits 24-28 == 01010
  if ((w & 0x1F000000) == 0x0A000000) {
    bool is64 = (w >> 31) & 1;
    int opc = (w >> 29) & 3;
    int N = (w >> 21) & 1;
    int rd = w & 0x1F, rn = (w >> 5) & 0x1F, rm = (w >> 16) & 0x1F;
    int imm6 = (w >> 10) & 0x3F;
    int shift = (w >> 22) & 3;
    // MOV = ORR Rd, XZR, Rm
    if (opc == 1 && N == 0 && rn == 31 && imm6 == 0 && shift == 0) {
      char o[32];
      std::snprintf(o, sizeof(o), "%s, %s", is64 ? reg_x(rd) : reg_w(rd),
                    is64 ? reg_x(rm) : reg_w(rm));
      set_insn(out, "mov", o);
      return;
    }
    // MVN = ORN Rd, XZR, Rm
    if (opc == 1 && N == 1 && rn == 31 && imm6 == 0) {
      char o[32];
      std::snprintf(o, sizeof(o), "%s, %s", is64 ? reg_x(rd) : reg_w(rd),
                    is64 ? reg_x(rm) : reg_w(rm));
      set_insn(out, "mvn", o);
      return;
    }
    const char* m = "and";
    if (opc == 0) m = N ? "bic" : "and";
    else if (opc == 1) m = N ? "orn" : "orr";
    else if (opc == 2) m = N ? "eon" : "eor";
    else m = N ? "bics" : "ands";
    char o[72];
    if (imm6)
      std::snprintf(o, sizeof(o), "%s, %s, %s, lsl #%d",
                    is64 ? reg_x(rd) : reg_w(rd), is64 ? reg_x(rn) : reg_w(rn),
                    is64 ? reg_x(rm) : reg_w(rm), imm6);
    else
      std::snprintf(o, sizeof(o), "%s, %s, %s", is64 ? reg_x(rd) : reg_w(rd),
                    is64 ? reg_x(rn) : reg_w(rn), is64 ? reg_x(rm) : reg_w(rm));
    set_insn(out, m, o);
    return;
  }
  // ADD/SUB imm
  if ((w & 0x1F000000) == 0x11000000 || (w & 0x1F000000) == 0x51000000) {
    bool is64 = (w >> 31) & 1;
    bool sub = ((w >> 30) & 1) != 0;
    int rd = w & 0x1F, rn = (w >> 5) & 0x1F;
    int imm = (w >> 10) & 0xFFF;
    int sh = (w >> 22) & 1;
    if (sh) imm <<= 12;
    char o[48];
    std::snprintf(o, sizeof(o), "%s, %s, #0x%X", is64 ? reg_x(rd) : reg_w(rd),
                  is64 ? reg_x(rn) : reg_w(rn), imm);
    set_insn(out, sub ? "sub" : "add", o);
    return;
  }
  // ADD/SUB/ADDS/SUBS shifted register (opc bits 29-30)
  if ((w & 0x1F200000) == 0x0B000000) {
    bool is64 = (w >> 31) & 1;
    int opc = (w >> 29) & 3;  // 0 add 1 adds 2 sub 3 subs
    int rd = w & 0x1F, rn = (w >> 5) & 0x1F, rm = (w >> 16) & 0x1F;
    char o[48];
    if (rd == 31 && opc == 3) {
      std::snprintf(o, sizeof(o), "%s, %s", is64 ? reg_x(rn) : reg_w(rn),
                    is64 ? reg_x(rm) : reg_w(rm));
      set_insn(out, "cmp", o);
      return;
    }
    if (rd == 31 && opc == 1) {
      std::snprintf(o, sizeof(o), "%s, %s", is64 ? reg_x(rn) : reg_w(rn),
                    is64 ? reg_x(rm) : reg_w(rm));
      set_insn(out, "cmn", o);
      return;
    }
    const char* m = "add";
    if (opc == 1) m = "adds";
    else if (opc == 2) m = "sub";
    else if (opc == 3) m = "subs";
    std::snprintf(o, sizeof(o), "%s, %s, %s", is64 ? reg_x(rd) : reg_w(rd),
                  is64 ? reg_x(rn) : reg_w(rn), is64 ? reg_x(rm) : reg_w(rm));
    set_insn(out, m, o);
    return;
  }
  // ADRP
  if ((w & 0x9F000000) == 0x90000000) {
    int rd = w & 0x1F;
    int64_t immhi = (w >> 5) & 0x7FFFF;
    int64_t immlo = (w >> 29) & 3;
    int64_t imm = sign_extend((immhi << 2) | immlo, 21) << 12;
    out.target = (addr & ~0xFFFull) + (uintptr_t)imm;
    char o[48];
    std::snprintf(o, sizeof(o), "%s, 0x%llX", reg_x(rd),
                  (unsigned long long)out.target);
    set_insn(out, "adrp", o);
    return;
  }
  // LDR/STR unsigned offset (imm12)
  // 111xx0x1 xx...
  if ((w & 0x3B000000) == 0x39000000 || (w & 0x3B000000) == 0x29000000) {
    bool is_load = ((w >> 22) & 1) != 0;
    int size = (w >> 30) & 3;  // 0=8 1=16 2=32 3=64
    int rt = w & 0x1F, rn = (w >> 5) & 0x1F;
    int imm = (w >> 10) & 0xFFF;
    int scale = size;
    imm <<= scale;
    const char* r =
        (size == 3) ? reg_x(rt) : (size >= 2 ? reg_w(rt) : reg_w(rt));
    char o[56];
    if (imm)
      std::snprintf(o, sizeof(o), "%s, [%s, #0x%X]", r, reg_x(rn), imm);
    else
      std::snprintf(o, sizeof(o), "%s, [%s]", r, reg_x(rn));
    const char* m = is_load ? (size == 3 ? "ldr" : (size == 2 ? "ldr" : "ldrb"))
                            : (size == 3 ? "str" : (size == 2 ? "str" : "strb"));
    if (size == 1) m = is_load ? "ldrh" : "strh";
    set_insn(out, m, o);
    return;
  }
  // STP/LDP pre/post/signed
  if ((w & 0x7E000000) == 0xA8000000 || (w & 0x7E000000) == 0xA9000000 ||
      (w & 0x7E000000) == 0xA9400000 || (w & 0x7E000000) == 0x29000000) {
    bool is64 = (w >> 31) & 1;
    bool load = ((w >> 22) & 1) != 0;
    int rt = w & 0x1F, rn = (w >> 5) & 0x1F, rt2 = (w >> 10) & 0x1F;
    int imm7 = (w >> 15) & 0x7F;
    int64_t imm = sign_extend(imm7, 7) * (is64 ? 8 : 4);
    char o[72];
    std::snprintf(o, sizeof(o), "%s, %s, [%s, #%lld]", is64 ? reg_x(rt) : reg_w(rt),
                  is64 ? reg_x(rt2) : reg_w(rt2), reg_x(rn), (long long)imm);
    set_insn(out, load ? "ldp" : "stp", o);
    return;
  }
  // CMP alias: SUBS XZR (imm or reg)
  if ((w & 0x1F00001F) == 0x7100001F || (w & 0x1F00001F) == 0x5100001F) {
    // SUBS Rd=XZR, Rn, #imm
    bool is64 = (w >> 31) & 1;
    int rn = (w >> 5) & 0x1F;
    int imm = (w >> 10) & 0xFFF;
    if ((w >> 22) & 1) imm <<= 12;
    char o[40];
    std::snprintf(o, sizeof(o), "%s, #0x%X", is64 ? reg_x(rn) : reg_w(rn), imm);
    set_insn(out, "cmp", o);
    return;
  }
  if ((w & 0x1F20001F) == 0x6B00001F || (w & 0x1F20001F) == 0x4B00001F) {
    bool is64 = (w >> 31) & 1;
    int rn = (w >> 5) & 0x1F, rm = (w >> 16) & 0x1F;
    char o[40];
    std::snprintf(o, sizeof(o), "%s, %s", is64 ? reg_x(rn) : reg_w(rn),
                  is64 ? reg_x(rm) : reg_w(rm));
    set_insn(out, "cmp", o);
    return;
  }
  // AND/ORR/EOR/ANDS immediate
  if ((w & 0x1F800000) == 0x12000000 || (w & 0x1F800000) == 0x32000000 ||
      (w & 0x1F800000) == 0x52000000 || (w & 0x1F800000) == 0x72000000) {
    bool is64 = (w >> 31) & 1;
    int opc = (w >> 29) & 3;
    int rd = w & 0x1F, rn = (w >> 5) & 0x1F;
    int immr = (w >> 16) & 0x3F, imms = (w >> 10) & 0x3F;
    const char* m = opc == 0 ? "and" : (opc == 1 ? "orr" : (opc == 2 ? "eor" : "ands"));
    char o[64];
    std::snprintf(o, sizeof(o), "%s, %s, #imm(r=%d,s=%d)",
                  is64 ? reg_x(rd) : reg_w(rd), is64 ? reg_x(rn) : reg_w(rn),
                  immr, imms);
    set_insn(out, m, o);
    return;
  }
  // UBFM / SBFM → lsl/lsr/asr/ubfx aliases
  if ((w & 0x7F800000) == 0x53000000 || (w & 0x7F800000) == 0x13000000) {
    bool is64 = (w >> 31) & 1;
    bool signed_ = ((w >> 29) & 3) == 0;  // SBFM
    int rd = w & 0x1F, rn = (w >> 5) & 0x1F;
    int immr = (w >> 16) & 0x3F, imms = (w >> 10) & 0x3F;
    int regsz = is64 ? 64 : 32;
    char o[56];
    // LSL: imms != regsz-1 && immr == imms+1  (simplified)
    if (!signed_ && imms != regsz - 1 && immr == imms + 1) {
      int sh = regsz - 1 - imms;
      std::snprintf(o, sizeof(o), "%s, %s, #%d", is64 ? reg_x(rd) : reg_w(rd),
                    is64 ? reg_x(rn) : reg_w(rn), sh);
      set_insn(out, "lsl", o);
      return;
    }
    // LSR: imms == regsz-1
    if (!signed_ && imms == regsz - 1) {
      std::snprintf(o, sizeof(o), "%s, %s, #%d", is64 ? reg_x(rd) : reg_w(rd),
                    is64 ? reg_x(rn) : reg_w(rn), immr);
      set_insn(out, "lsr", o);
      return;
    }
    // ASR: SBFM imms == regsz-1
    if (signed_ && imms == regsz - 1) {
      std::snprintf(o, sizeof(o), "%s, %s, #%d", is64 ? reg_x(rd) : reg_w(rd),
                    is64 ? reg_x(rn) : reg_w(rn), immr);
      set_insn(out, "asr", o);
      return;
    }
    // UXTW / SXTW-like
    if (!signed_ && immr == 0 && imms == 31 && is64) {
      std::snprintf(o, sizeof(o), "%s, %s", reg_x(rd), reg_w(rn));
      set_insn(out, "uxtw", o);
      return;
    }
    if (signed_ && immr == 0 && imms == 31) {
      std::snprintf(o, sizeof(o), "%s, %s", is64 ? reg_x(rd) : reg_w(rd),
                    reg_w(rn));
      set_insn(out, "sxtw", o);
      return;
    }
    std::snprintf(o, sizeof(o), "%s, %s, #%d, #%d",
                  is64 ? reg_x(rd) : reg_w(rd), is64 ? reg_x(rn) : reg_w(rn),
                  immr, imms);
    set_insn(out, signed_ ? "sbfm" : "ubfm", o);
    return;
  }
  // MADD / MSUB / MUL alias
  if ((w & 0x1F000000) == 0x1B000000) {
    bool is64 = (w >> 31) & 1;
    int rd = w & 0x1F, rn = (w >> 5) & 0x1F, ra = (w >> 10) & 0x1F,
        rm = (w >> 16) & 0x1F;
    bool sub = ((w >> 15) & 1) != 0;
    char o[64];
    if (ra == 31 && !sub) {
      std::snprintf(o, sizeof(o), "%s, %s, %s", is64 ? reg_x(rd) : reg_w(rd),
                    is64 ? reg_x(rn) : reg_w(rn), is64 ? reg_x(rm) : reg_w(rm));
      set_insn(out, "mul", o);
    } else {
      std::snprintf(o, sizeof(o), "%s, %s, %s, %s",
                    is64 ? reg_x(rd) : reg_w(rd), is64 ? reg_x(rn) : reg_w(rn),
                    is64 ? reg_x(rm) : reg_w(rm), is64 ? reg_x(ra) : reg_w(ra));
      set_insn(out, sub ? "msub" : "madd", o);
    }
    return;
  }
  // TBZ / TBNZ
  if ((w & 0x7F000000) == 0x36000000 || (w & 0x7F000000) == 0x37000000) {
    bool nz = ((w >> 24) & 1) != 0;
    int rt = w & 0x1F;
    int bit = ((w >> 31) << 5) | ((w >> 19) & 0x1F);
    int64_t imm = sign_extend((w >> 5) & 0x3FFF, 14) << 2;
    out.target = addr + (uintptr_t)imm;
    char o[56];
    std::snprintf(o, sizeof(o), "%s, #%d, 0x%llX", reg_x(rt), bit,
                  (unsigned long long)out.target);
    set_insn(out, nz ? "tbnz" : "tbz", o);
    out.is_branch = true;
    out.is_cond = true;
    return;
  }
  // LDR literal
  if ((w & 0x3B000000) == 0x18000000) {
    int rt = w & 0x1F;
    int opc = (w >> 30) & 3;
    int64_t imm = sign_extend((w >> 5) & 0x7FFFF, 19) << 2;
    out.target = addr + (uintptr_t)imm;
    char o[48];
    std::snprintf(o, sizeof(o), "%s, 0x%llX",
                  opc == 1 ? reg_x(rt) : reg_w(rt),
                  (unsigned long long)out.target);
    set_insn(out, "ldr", o);
    return;
  }
  // LDUR / STUR unscaled
  if ((w & 0x3B200C00) == 0x38000000 || (w & 0x3B200C00) == 0x38000400) {
    bool is_load = ((w >> 22) & 1) != 0;
    int size = (w >> 30) & 3;
    int rt = w & 0x1F, rn = (w >> 5) & 0x1F;
    int64_t imm = sign_extend((w >> 12) & 0x1FF, 9);
    const char* r = size == 3 ? reg_x(rt) : reg_w(rt);
    char o[56];
    std::snprintf(o, sizeof(o), "%s, [%s, #%lld]", r, reg_x(rn), (long long)imm);
    const char* m = is_load ? (size == 0 ? "ldurb" : (size == 1 ? "ldurh" : "ldur"))
                            : (size == 0 ? "sturb" : (size == 1 ? "sturh" : "stur"));
    set_insn(out, m, o);
    return;
  }
  // CSEL / CSINC / CSINV / CSNEG
  if ((w & 0x1FE00000) == 0x1A800000) {
    bool is64 = (w >> 31) & 1;
    int op = (w >> 29) & 3;
    int rd = w & 0x1F, rn = (w >> 5) & 0x1F, rm = (w >> 16) & 0x1F;
    int cond = (w >> 12) & 0xF;
    int op2 = (w >> 10) & 3;
    static const char* cc[] = {"eq","ne","cs","cc","mi","pl","vs","vc",
                               "hi","ls","ge","lt","gt","le","al","nv"};
    char o[72];
    // CSET: CSINC Rd, XZR, XZR, invcond
    if (op == 0 && op2 == 1 && rn == 31 && rm == 31) {
      std::snprintf(o, sizeof(o), "%s, %s", is64 ? reg_x(rd) : reg_w(rd),
                    cc[cond ^ 1]);
      set_insn(out, "cset", o);
      return;
    }
    const char* m = "csel";
    if (op2 == 0) m = "csel";
    else if (op2 == 1) m = "csinc";
    else if (op2 == 2) m = "csinv";
    else m = "csneg";
    (void)op;
    std::snprintf(o, sizeof(o), "%s, %s, %s, %s", is64 ? reg_x(rd) : reg_w(rd),
                  is64 ? reg_x(rn) : reg_w(rn), is64 ? reg_x(rm) : reg_w(rm),
                  cc[cond]);
    set_insn(out, m, o);
    return;
  }
  // NEG alias: SUB Rd, XZR, Rm
  if ((w & 0x7F2003E0) == 0x4B0003E0 || (w & 0x7F2003E0) == 0x0B0003E0) {
    bool is64 = (w >> 31) & 1;
    int rd = w & 0x1F, rm = (w >> 16) & 0x1F;
    char o[32];
    std::snprintf(o, sizeof(o), "%s, %s", is64 ? reg_x(rd) : reg_w(rd),
                  is64 ? reg_x(rm) : reg_w(rm));
    set_insn(out, "neg", o);
    return;
  }
  // HINT / NOP family already; PACIASP etc
  if ((w & 0xFFFFF0FF) == 0xD503201F || w == 0xD503233F || w == 0xD50323BF ||
      w == 0xD503237F || w == 0xD50323FF) {
    if (w == 0xD503233F) set_insn(out, "paciasp", "");
    else if (w == 0xD50323BF) set_insn(out, "autiasp", "");
    else if (w == 0xD503237F) set_insn(out, "pacibsp", "");
    else if (w == 0xD50323FF) set_insn(out, "autibsp", "");
    else set_insn(out, "hint", "");
    return;
  }
  // BTI
  if ((w & 0xFFFFFF3F) == 0xD503241F) {
    set_insn(out, "bti", "");
    return;
  }
  // SVC
  if ((w & 0xFFE0001F) == 0xD4000001) {
    int imm = (w >> 5) & 0xFFFF;
    char o[24];
    std::snprintf(o, sizeof(o), "#%d", imm);
    set_insn(out, "svc", o);
    return;
  }
  // BRK
  if ((w & 0xFFE0001F) == 0xD4200000) {
    int imm = (w >> 5) & 0xFFFF;
    char o[24];
    std::snprintf(o, sizeof(o), "#%d", imm);
    set_insn(out, "brk", o);
    return;
  }
  // ADR
  if ((w & 0x9F000000) == 0x10000000) {
    int rd = w & 0x1F;
    int64_t immhi = (w >> 5) & 0x7FFFF;
    int64_t immlo = (w >> 29) & 3;
    int64_t imm = sign_extend((immhi << 2) | immlo, 21);
    out.target = addr + (uintptr_t)imm;
    char o[48];
    std::snprintf(o, sizeof(o), "%s, 0x%llX", reg_x(rd),
                  (unsigned long long)out.target);
    set_insn(out, "adr", o);
    return;
  }
  // ADDS/SUBS imm (flags; not just cmp alias)
  if ((w & 0x1F000000) == 0x31000000 || (w & 0x1F000000) == 0x71000000) {
    bool is64 = (w >> 31) & 1;
    bool sub = ((w >> 30) & 1) != 0;
    int rd = w & 0x1F, rn = (w >> 5) & 0x1F;
    int imm = (w >> 10) & 0xFFF;
    if ((w >> 22) & 1) imm <<= 12;
    if (rd == 31) {
      char o[40];
      std::snprintf(o, sizeof(o), "%s, #0x%X", is64 ? reg_x(rn) : reg_w(rn),
                    imm);
      set_insn(out, sub ? "cmp" : "cmn", o);
      return;
    }
    char o[48];
    std::snprintf(o, sizeof(o), "%s, %s, #0x%X", is64 ? reg_x(rd) : reg_w(rd),
                  is64 ? reg_x(rn) : reg_w(rn), imm);
    set_insn(out, sub ? "subs" : "adds", o);
    return;
  }
  // ADD/SUB extended register
  if ((w & 0x1FE00000) == 0x0B200000 || (w & 0x1FE00000) == 0x4B200000) {
    bool is64 = (w >> 31) & 1;
    bool sub = ((w >> 30) & 1) != 0;
    int rd = w & 0x1F, rn = (w >> 5) & 0x1F, rm = (w >> 16) & 0x1F;
    int option = (w >> 13) & 7;
    int imm3 = (w >> 10) & 7;
    static const char* ext[] = {"uxtb", "uxth", "uxtw", "uxtx",
                                "sxtb", "sxth", "sxtw", "sxtx"};
    char o[72];
    if (imm3)
      std::snprintf(o, sizeof(o), "%s, %s, %s, %s #%d",
                    is64 ? reg_x(rd) : reg_w(rd), is64 ? reg_x(rn) : reg_w(rn),
                    is64 ? reg_x(rm) : reg_w(rm), ext[option], imm3);
    else
      std::snprintf(o, sizeof(o), "%s, %s, %s, %s",
                    is64 ? reg_x(rd) : reg_w(rd), is64 ? reg_x(rn) : reg_w(rn),
                    is64 ? reg_x(rm) : reg_w(rm), ext[option]);
    set_insn(out, sub ? "sub" : "add", o);
    return;
  }
  // UDIV / SDIV
  if ((w & 0x1FE0FC00) == 0x1AC00800 || (w & 0x1FE0FC00) == 0x1AC00C00) {
    bool is64 = (w >> 31) & 1;
    bool signed_ = ((w >> 10) & 3) == 3;
    int rd = w & 0x1F, rn = (w >> 5) & 0x1F, rm = (w >> 16) & 0x1F;
    char o[48];
    std::snprintf(o, sizeof(o), "%s, %s, %s", is64 ? reg_x(rd) : reg_w(rd),
                  is64 ? reg_x(rn) : reg_w(rn), is64 ? reg_x(rm) : reg_w(rm));
    set_insn(out, signed_ ? "sdiv" : "udiv", o);
    return;
  }
  // LSLV/LSRV/ASRV/RORV
  if ((w & 0x1FE0F800) == 0x1AC02000) {
    bool is64 = (w >> 31) & 1;
    int op2 = (w >> 10) & 3;
    int rd = w & 0x1F, rn = (w >> 5) & 0x1F, rm = (w >> 16) & 0x1F;
    const char* m =
        op2 == 0 ? "lsl" : (op2 == 1 ? "lsr" : (op2 == 2 ? "asr" : "ror"));
    char o[48];
    std::snprintf(o, sizeof(o), "%s, %s, %s", is64 ? reg_x(rd) : reg_w(rd),
                  is64 ? reg_x(rn) : reg_w(rn), is64 ? reg_x(rm) : reg_w(rm));
    set_insn(out, m, o);
    return;
  }
  // EXTR / ROR alias
  if ((w & 0x1F800000) == 0x13800000) {
    bool is64 = (w >> 31) & 1;
    int rd = w & 0x1F, rn = (w >> 5) & 0x1F, rm = (w >> 16) & 0x1F;
    int imms = (w >> 10) & 0x3F;
    char o[56];
    if (rn == rm) {
      std::snprintf(o, sizeof(o), "%s, %s, #%d", is64 ? reg_x(rd) : reg_w(rd),
                    is64 ? reg_x(rn) : reg_w(rn), imms);
      set_insn(out, "ror", o);
    } else {
      std::snprintf(o, sizeof(o), "%s, %s, %s, #%d",
                    is64 ? reg_x(rd) : reg_w(rd), is64 ? reg_x(rn) : reg_w(rn),
                    is64 ? reg_x(rm) : reg_w(rm), imms);
      set_insn(out, "extr", o);
    }
    return;
  }
  // CLZ / CLS / RBIT / REV*
  if ((w & 0x5FFFFC00) == 0x5AC01000 || (w & 0x5FFFFC00) == 0x5AC01400 ||
      (w & 0x5FFFFC00) == 0x5AC00000 || (w & 0x5FFFFC00) == 0x5AC00800 ||
      (w & 0x5FFFFC00) == 0x5AC00C00) {
    bool is64 = (w >> 31) & 1;
    int opc = (w >> 10) & 0x3F;
    int rd = w & 0x1F, rn = (w >> 5) & 0x1F;
    const char* m = "clz";
    if (opc == 0) m = "rbit";
    else if (opc == 2) m = "rev";
    else if (opc == 3) m = "rev16";
    else if (opc == 4) m = "clz";
    else if (opc == 5) m = "cls";
    else if (opc == 1) m = is64 ? "rev32" : "rev";
    char o[32];
    std::snprintf(o, sizeof(o), "%s, %s", is64 ? reg_x(rd) : reg_w(rd),
                  is64 ? reg_x(rn) : reg_w(rn));
    set_insn(out, m, o);
    return;
  }
  // LDR/STR register offset
  if ((w & 0x3B200C00) == 0x38200800 || (w & 0x3B200C00) == 0x38600800) {
    bool is_load = ((w >> 22) & 1) != 0;
    int size = (w >> 30) & 3;
    int rt = w & 0x1F, rn = (w >> 5) & 0x1F, rm = (w >> 16) & 0x1F;
    int option = (w >> 13) & 7;
    int S = (w >> 12) & 1;
    const char* r = size == 3 ? reg_x(rt) : reg_w(rt);
    char o[64];
    if (S || option != 3)
      std::snprintf(o, sizeof(o), "%s, [%s, %s, ext]", r, reg_x(rn),
                    reg_x(rm));
    else
      std::snprintf(o, sizeof(o), "%s, [%s, %s]", r, reg_x(rn), reg_x(rm));
    const char* m = is_load ? (size == 0   ? "ldrb"
                               : size == 1 ? "ldrh"
                                           : "ldr")
                            : (size == 0   ? "strb"
                               : size == 1 ? "strh"
                                           : "str");
    set_insn(out, m, o);
    return;
  }
  // LDR/STR pre/post-index (imm9)
  if ((w & 0x3B200C00) == 0x38000400 || (w & 0x3B200C00) == 0x38000C00) {
    bool is_load = ((w >> 22) & 1) != 0;
    bool pre = ((w >> 11) & 1) != 0;
    int size = (w >> 30) & 3;
    int rt = w & 0x1F, rn = (w >> 5) & 0x1F;
    int64_t imm = sign_extend((w >> 12) & 0x1FF, 9);
    const char* r = size == 3 ? reg_x(rt) : reg_w(rt);
    char o[64];
    if (pre)
      std::snprintf(o, sizeof(o), "%s, [%s, #%lld]!", r, reg_x(rn),
                    (long long)imm);
    else
      std::snprintf(o, sizeof(o), "%s, [%s], #%lld", r, reg_x(rn),
                    (long long)imm);
    const char* m = is_load ? (size == 0 ? "ldrb" : (size == 1 ? "ldrh" : "ldr"))
                            : (size == 0 ? "strb" : (size == 1 ? "strh" : "str"));
    set_insn(out, m, o);
    return;
  }
  // LDRSW (unsigned / literal-ish signed word)
  if ((w & 0xFFC00000) == 0xB9800000) {
    int rt = w & 0x1F, rn = (w >> 5) & 0x1F;
    int imm = ((w >> 10) & 0xFFF) << 2;
    char o[56];
    if (imm)
      std::snprintf(o, sizeof(o), "%s, [%s, #0x%X]", reg_x(rt), reg_x(rn), imm);
    else
      std::snprintf(o, sizeof(o), "%s, [%s]", reg_x(rt), reg_x(rn));
    set_insn(out, "ldrsw", o);
    return;
  }
  // MRS / MSR (system registers, simplified encoding display)
  if ((w & 0xFFF00000) == 0xD5300000 || (w & 0xFFF00000) == 0xD5100000) {
    bool is_mrs = ((w >> 21) & 1) != 0;
    int rt = w & 0x1F;
    int sys = (w >> 5) & 0x7FFF;
    char o[48];
    if (is_mrs)
      std::snprintf(o, sizeof(o), "%s, sreg_0x%X", reg_x(rt), sys);
    else
      std::snprintf(o, sizeof(o), "sreg_0x%X, %s", sys, reg_x(rt));
    set_insn(out, is_mrs ? "mrs" : "msr", o);
    return;
  }
  // DSB / DMB / ISB
  if ((w & 0xFFFFF0FF) == 0xD503309F || (w & 0xFFFFF0FF) == 0xD50330BF ||
      (w & 0xFFFFF0FF) == 0xD50330DF) {
    int crm = (w >> 8) & 0xF;
    char o[16];
    std::snprintf(o, sizeof(o), "#%d", crm);
    if ((w & 0xFF) == 0x9F) set_insn(out, "dsb", o);
    else if ((w & 0xFF) == 0xBF) set_insn(out, "dmb", o);
    else set_insn(out, "isb", o);
    return;
  }
  // CCMP / CCMN register
  if ((w & 0x1FE00C10) == 0x3A400000 || (w & 0x1FE00C10) == 0x3A400010) {
    bool is64 = (w >> 31) & 1;
    bool cmn = ((w >> 30) & 1) == 0;
    int rn = (w >> 5) & 0x1F, rm = (w >> 16) & 0x1F;
    int nzcv = w & 0xF;
    int cond = (w >> 12) & 0xF;
    static const char* cc[] = {"eq","ne","cs","cc","mi","pl","vs","vc",
                               "hi","ls","ge","lt","gt","le","al","nv"};
    char o[64];
    std::snprintf(o, sizeof(o), "%s, %s, #%d, %s",
                  is64 ? reg_x(rn) : reg_w(rn), is64 ? reg_x(rm) : reg_w(rm),
                  nzcv, cc[cond]);
    set_insn(out, cmn ? "ccmn" : "ccmp", o);
    return;
  }
  // TST alias: ANDS XZR
  if ((w & 0x1F00001F) == 0x6A00001F || (w & 0x1F00001F) == 0x7200001F) {
    bool is64 = (w >> 31) & 1;
    int rn = (w >> 5) & 0x1F;
    if ((w & 0x1F800000) == 0x72000000) {
      int immr = (w >> 16) & 0x3F, imms = (w >> 10) & 0x3F;
      char o[48];
      std::snprintf(o, sizeof(o), "%s, #imm(r=%d,s=%d)",
                    is64 ? reg_x(rn) : reg_w(rn), immr, imms);
      set_insn(out, "tst", o);
      return;
    }
    int rm = (w >> 16) & 0x1F;
    char o[40];
    std::snprintf(o, sizeof(o), "%s, %s", is64 ? reg_x(rn) : reg_w(rn),
                  is64 ? reg_x(rm) : reg_w(rm));
    set_insn(out, "tst", o);
    return;
  }
  // Scalar FP: FMOV (register), FADD/FSUB/FMUL/FDIV, FCMP, FCVT
  // FMOV Sd,Sn / Dd,Dn  — 1E204000 pattern
  if ((w & 0xFF3FFC00) == 0x1E204000) {
    bool is_d = ((w >> 22) & 1) != 0;
    int rd = w & 0x1F, rn = (w >> 5) & 0x1F;
    char o[24];
    std::snprintf(o, sizeof(o), "%s, %s", is_d ? reg_d(rd) : reg_s(rd),
                  is_d ? reg_d(rn) : reg_s(rn));
    set_insn(out, "fmov", o);
    return;
  }
  // FMOV Xd, Dn / Wn, Sn
  if ((w & 0xFF3FFC00) == 0x1E260000 || (w & 0xFF3FFC00) == 0x1E270000) {
    bool to_fp = ((w >> 16) & 1) != 0;
    bool is_d = ((w >> 22) & 1) != 0;
    int rd = w & 0x1F, rn = (w >> 5) & 0x1F;
    char o[24];
    if (to_fp)
      std::snprintf(o, sizeof(o), "%s, %s", is_d ? reg_d(rd) : reg_s(rd),
                    is_d ? reg_x(rn) : reg_w(rn));
    else
      std::snprintf(o, sizeof(o), "%s, %s", is_d ? reg_x(rd) : reg_w(rd),
                    is_d ? reg_d(rn) : reg_s(rn));
    set_insn(out, "fmov", o);
    return;
  }
  // FADD/FSUB/FMUL/FDIV 2-src
  if ((w & 0xFF20FC00) == 0x1E202800 || (w & 0xFF20FC00) == 0x1E203800 ||
      (w & 0xFF20FC00) == 0x1E200800 || (w & 0xFF20FC00) == 0x1E201800) {
    bool is_d = ((w >> 22) & 1) != 0;
    int opc = (w >> 10) & 0x3F;
    int rd = w & 0x1F, rn = (w >> 5) & 0x1F, rm = (w >> 16) & 0x1F;
    const char* m = "fadd";
    if (opc == 0x0A) m = "fmul";
    else if (opc == 0x0E) m = "fdiv";
    else if (opc == 0x0A) m = "fmul";
    else if (opc == 0x0E) m = "fdiv";
    else if (opc == 0x0A) m = "fmul";
    // opcode bits: fmul=0x08? check ARM: ftype | opcode
    // Better: use bits 12-15
    int op = (w >> 12) & 0xF;
    if (op == 2) m = "fadd";
    else if (op == 3) m = "fsub";
    else if (op == 0) m = "fmul";
    else if (op == 1) m = "fdiv";
    char o[40];
    std::snprintf(o, sizeof(o), "%s, %s, %s", is_d ? reg_d(rd) : reg_s(rd),
                  is_d ? reg_d(rn) : reg_s(rn), is_d ? reg_d(rm) : reg_s(rm));
    set_insn(out, m, o);
    return;
  }
  // FCMP
  if ((w & 0xFF20FC1F) == 0x1E202000 || (w & 0xFF20FC1F) == 0x1E202008) {
    bool is_d = ((w >> 22) & 1) != 0;
    int rn = (w >> 5) & 0x1F, rm = (w >> 16) & 0x1F;
    char o[32];
    if (((w >> 3) & 1) == 0 && rm == 0)
      std::snprintf(o, sizeof(o), "%s, #0.0", is_d ? reg_d(rn) : reg_s(rn));
    else
      std::snprintf(o, sizeof(o), "%s, %s", is_d ? reg_d(rn) : reg_s(rn),
                    is_d ? reg_d(rm) : reg_s(rm));
    set_insn(out, "fcmp", o);
    return;
  }
  // FCVTZS / SCVTF (scalar, simplified)
  if ((w & 0xFF3FFC00) == 0x1E380000 || (w & 0xFF3FFC00) == 0x1E220000) {
    bool is_d = ((w >> 22) & 1) != 0;
    bool to_int = ((w & 0xFF3FFC00) == 0x1E380000);
    int rd = w & 0x1F, rn = (w >> 5) & 0x1F;
    char o[32];
    if (to_int)
      std::snprintf(o, sizeof(o), "%s, %s", is_d ? reg_x(rd) : reg_w(rd),
                    is_d ? reg_d(rn) : reg_s(rn));
    else
      std::snprintf(o, sizeof(o), "%s, %s", is_d ? reg_d(rd) : reg_s(rd),
                    is_d ? reg_x(rn) : reg_w(rn));
    set_insn(out, to_int ? "fcvtzs" : "scvtf", o);
    return;
  }
  // PRFM
  if ((w & 0xFFC00000) == 0xF9800000) {
    int rt = w & 0x1F, rn = (w >> 5) & 0x1F;
    int imm = ((w >> 10) & 0xFFF) << 3;
    char o[48];
    std::snprintf(o, sizeof(o), "pld, [%s, #0x%X]", reg_x(rn), imm);
    (void)rt;
    set_insn(out, "prfm", o);
    return;
  }
  // UDF
  if ((w >> 16) == 0) {
    char o[24];
    std::snprintf(o, sizeof(o), "#0x%X", w & 0xFFFF);
    set_insn(out, "udf", o);
    return;
  }

  // fallback：仍给出可读伪指令，避免空伪C
  char o[24];
  std::snprintf(o, sizeof(o), "0x%08X", w);
  set_insn(out, ".word", o);
}

}  // namespace

// 去掉 ops 里多余空格，便于解析
static void trim_copy(const char* src, char* dst, size_t cap) {
  if (!src || cap < 2) {
    if (dst && cap) dst[0] = 0;
    return;
  }
  while (*src == ' ') src++;
  size_t n = 0;
  while (src[n] && n + 1 < cap) {
    dst[n] = src[n];
    n++;
  }
  dst[n] = 0;
  while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == ',')) dst[--n] = 0;
}

/** 按逗号切分操作数，但方括号 [] 内的逗号不算分隔 */
static int split_ops(const char* o, char out[][64], int maxn) {
  int n = 0;
  int depth = 0;
  size_t start = 0, i = 0;
  auto flush = [&](size_t end) {
    if (n >= maxn) return;
    while (start < end && o[start] == ' ') start++;
    size_t len = end > start ? end - start : 0;
    while (len > 0 && (o[start + len - 1] == ' ' || o[start + len - 1] == ','))
      len--;
    if (len >= 64) len = 63;
    if (len == 0) return;
    std::memcpy(out[n], o + start, len);
    out[n][len] = 0;
    n++;
  };
  for (; o[i]; ++i) {
    if (o[i] == '[') depth++;
    else if (o[i] == ']') depth = depth > 0 ? depth - 1 : 0;
    else if (o[i] == ',' && depth == 0) {
      flush(i);
      start = i + 1;
    }
  }
  flush(i);
  return n;
}

void insn_to_pseudo(Insn& insn) {
  insn.pseudo[0] = 0;
  const char* m = insn.mnem;
  const char* o = insn.ops;
  char ops[96]{};
  trim_copy(o, ops, sizeof(ops));
  o = ops;

  auto fallback = [&]() {
    // 完整可阅读的默认形式，避免只剩机器码
    if (o[0])
      std::snprintf(insn.pseudo, sizeof(insn.pseudo), "asm(\"%s %s\");", m, o);
    else
      std::snprintf(insn.pseudo, sizeof(insn.pseudo), "asm(\"%s\");", m);
  };

  if (std::strcmp(m, "nop") == 0) {
    std::snprintf(insn.pseudo, sizeof(insn.pseudo), "; nop");
    return;
  }
  if (std::strcmp(m, "ret") == 0) {
    std::snprintf(insn.pseudo, sizeof(insn.pseudo), "return;");
    return;
  }
  if (std::strcmp(m, "bl") == 0) {
    std::snprintf(insn.pseudo, sizeof(insn.pseudo),
                  "x0 = call_%s(/* args in x0-x7 */);", o[0] ? o : "fn");
    return;
  }
  if (std::strcmp(m, "blr") == 0) {
    std::snprintf(insn.pseudo, sizeof(insn.pseudo),
                  "x0 = (*%s)(/* args */);", o[0] ? o : "fn");
    return;
  }
  if (std::strcmp(m, "br") == 0) {
    std::snprintf(insn.pseudo, sizeof(insn.pseudo), "goto *%s;", o);
    return;
  }
  if (std::strcmp(m, "b") == 0) {
    std::snprintf(insn.pseudo, sizeof(insn.pseudo), "goto %s;", o);
    return;
  }
  if (std::strncmp(m, "b.", 2) == 0) {
    std::snprintf(insn.pseudo, sizeof(insn.pseudo),
                  "if (cond_%s) goto %s;", m + 2, o);
    return;
  }
  if (std::strcmp(m, "cbz") == 0 || std::strcmp(m, "cbnz") == 0) {
    char reg[24]{}, tgt[40]{};
    if (std::sscanf(o, "%23[^,], %39s", reg, tgt) >= 2)
      std::snprintf(insn.pseudo, sizeof(insn.pseudo),
                    "if (%s %s 0) goto %s;", reg, m[2] == 'n' ? "!=" : "==",
                    tgt);
    else
      fallback();
    return;
  }
  if (std::strcmp(m, "mov") == 0 || std::strcmp(m, "movz") == 0 ||
      std::strcmp(m, "movn") == 0 || std::strcmp(m, "movk") == 0) {
    char a[32]{}, b[64]{};
    // movk Rd, #imm, lsl #N
    if (std::sscanf(o, "%31[^,], %63[^,]", a, b) >= 2) {
      // 去掉 b 尾部可能的 ", lsl #n"
      char* lsl = std::strstr(b, ", lsl");
      if (lsl) *lsl = 0;
      if (std::strcmp(m, "movk") == 0)
        std::snprintf(insn.pseudo, sizeof(insn.pseudo),
                      "%s = (%s & ~mask) | %s;", a, a, b);
      else if (std::strcmp(m, "movn") == 0)
        std::snprintf(insn.pseudo, sizeof(insn.pseudo), "%s = ~(%s);", a, b);
      else
        std::snprintf(insn.pseudo, sizeof(insn.pseudo), "%s = %s;", a, b);
    } else
      fallback();
    return;
  }
  if (std::strcmp(m, "add") == 0 || std::strcmp(m, "sub") == 0 ||
      std::strcmp(m, "orr") == 0 || std::strcmp(m, "and") == 0 ||
      std::strcmp(m, "eor") == 0 || std::strcmp(m, "mul") == 0 ||
      std::strcmp(m, "udiv") == 0 || std::strcmp(m, "sdiv") == 0 ||
      std::strcmp(m, "bic") == 0 || std::strcmp(m, "orn") == 0 ||
      std::strcmp(m, "eon") == 0 || std::strcmp(m, "ands") == 0 ||
      std::strcmp(m, "bics") == 0) {
    char parts[4][64]{};
    int pn = split_ops(o, parts, 4);
    if (pn >= 3) {
      const char* d = parts[0];
      const char* a = parts[1];
      const char* b = parts[2];
      const char* op = "+";
      if (std::strcmp(m, "sub") == 0) op = "-";
      else if (std::strcmp(m, "orr") == 0 || std::strcmp(m, "orn") == 0) op = "|";
      else if (std::strcmp(m, "and") == 0 || std::strcmp(m, "ands") == 0 ||
               std::strcmp(m, "bic") == 0 || std::strcmp(m, "bics") == 0)
        op = "&";
      else if (std::strcmp(m, "eor") == 0 || std::strcmp(m, "eon") == 0) op = "^";
      else if (std::strcmp(m, "mul") == 0) op = "*";
      else if (std::strcmp(m, "udiv") == 0 || std::strcmp(m, "sdiv") == 0)
        op = "/";
      if (std::strcmp(m, "bic") == 0 || std::strcmp(m, "bics") == 0)
        std::snprintf(insn.pseudo, sizeof(insn.pseudo), "%s = %s & ~(%s);", d,
                      a, b);
      else if (std::strcmp(m, "orn") == 0)
        std::snprintf(insn.pseudo, sizeof(insn.pseudo), "%s = %s | ~(%s);", d,
                      a, b);
      else if (std::strcmp(m, "eon") == 0)
        std::snprintf(insn.pseudo, sizeof(insn.pseudo), "%s = %s ^ ~(%s);", d,
                      a, b);
      else if (std::strcmp(m, "ands") == 0)
        std::snprintf(insn.pseudo, sizeof(insn.pseudo),
                      "%s = %s & %s; flags = NZCV;", d, a, b);
      else
        std::snprintf(insn.pseudo, sizeof(insn.pseudo), "%s = %s %s %s;", d, a,
                      op, b);
    } else if (pn >= 2) {
      std::snprintf(insn.pseudo, sizeof(insn.pseudo), "%s = %s(%s);", parts[0],
                    m, parts[1]);
    } else
      fallback();
    return;
  }
  if (std::strcmp(m, "neg") == 0 || std::strcmp(m, "mvn") == 0) {
    char d[24]{}, a[40]{};
    if (std::sscanf(o, "%23[^,], %39s", d, a) >= 2)
      std::snprintf(insn.pseudo, sizeof(insn.pseudo), "%s = %s%s;", d,
                    std::strcmp(m, "neg") == 0 ? "-" : "~", a);
    else
      fallback();
    return;
  }
  if (std::strcmp(m, "madd") == 0 || std::strcmp(m, "msub") == 0) {
    char d[20]{}, a[20]{}, b[20]{}, c[20]{};
    if (std::sscanf(o, "%19[^,], %19[^,], %19[^,], %19s", d, a, b, c) >= 4)
      std::snprintf(insn.pseudo, sizeof(insn.pseudo),
                    std::strcmp(m, "madd") == 0 ? "%s = %s + %s * %s;"
                                                : "%s = %s - %s * %s;",
                    d, c, a, b);
    else
      fallback();
    return;
  }
  if (std::strcmp(m, "tbz") == 0 || std::strcmp(m, "tbnz") == 0) {
    char reg[24]{}, bit[16]{}, tgt[40]{};
    if (std::sscanf(o, "%23[^,], %15[^,], %39s", reg, bit, tgt) >= 3)
      std::snprintf(insn.pseudo, sizeof(insn.pseudo),
                    "if (((%s >> %s) & 1) %s 0) goto %s;", reg, bit,
                    m[2] == 'n' ? "!=" : "==", tgt);
    else
      fallback();
    return;
  }
  if (std::strcmp(m, "cset") == 0) {
    char d[24]{}, cc[16]{};
    if (std::sscanf(o, "%23[^,], %15s", d, cc) >= 2)
      std::snprintf(insn.pseudo, sizeof(insn.pseudo),
                    "%s = cond_%s ? 1 : 0;", d, cc);
    else
      fallback();
    return;
  }
  if (std::strcmp(m, "csel") == 0 || std::strcmp(m, "csinc") == 0 ||
      std::strcmp(m, "csinv") == 0 || std::strcmp(m, "csneg") == 0) {
    char d[20]{}, a[20]{}, b[20]{}, cc[16]{};
    if (std::sscanf(o, "%19[^,], %19[^,], %19[^,], %15s", d, a, b, cc) >= 4) {
      if (std::strcmp(m, "csel") == 0)
        std::snprintf(insn.pseudo, sizeof(insn.pseudo),
                      "%s = cond_%s ? %s : %s;", d, cc, a, b);
      else if (std::strcmp(m, "csinc") == 0)
        std::snprintf(insn.pseudo, sizeof(insn.pseudo),
                      "%s = cond_%s ? %s : (%s+1);", d, cc, a, b);
      else if (std::strcmp(m, "csinv") == 0)
        std::snprintf(insn.pseudo, sizeof(insn.pseudo),
                      "%s = cond_%s ? %s : ~%s;", d, cc, a, b);
      else
        std::snprintf(insn.pseudo, sizeof(insn.pseudo),
                      "%s = cond_%s ? %s : -%s;", d, cc, a, b);
    } else
      fallback();
    return;
  }
  if (std::strcmp(m, "uxtw") == 0 || std::strcmp(m, "sxtw") == 0 ||
      std::strcmp(m, "uxth") == 0 || std::strcmp(m, "sxth") == 0 ||
      std::strcmp(m, "uxtb") == 0 || std::strcmp(m, "sxtb") == 0) {
    char d[24]{}, a[40]{};
    if (std::sscanf(o, "%23[^,], %39s", d, a) >= 2)
      std::snprintf(insn.pseudo, sizeof(insn.pseudo), "%s = (%s)%s;", d, m, a);
    else
      fallback();
    return;
  }
  if (std::strcmp(m, "ubfm") == 0 || std::strcmp(m, "sbfm") == 0) {
    char d[24]{}, a[24]{}, r[16]{}, s[16]{};
    if (std::sscanf(o, "%23[^,], %23[^,], %15[^,], %15s", d, a, r, s) >= 4)
      std::snprintf(insn.pseudo, sizeof(insn.pseudo),
                    "%s = bitfield_%s(%s, r=%s, s=%s);", d, m, a, r, s);
    else
      fallback();
    return;
  }
  if (std::strncmp(m, "ldr", 3) == 0 || std::strncmp(m, "ldur", 4) == 0) {
    char parts[4][64]{};
    int pn = split_ops(o, parts, 4);
    if (pn >= 2) {
      const char* cast = "";
      if (std::strcmp(m, "ldrb") == 0 || std::strcmp(m, "ldurb") == 0)
        cast = "(uint8_t)";
      else if (std::strcmp(m, "ldrh") == 0 || std::strcmp(m, "ldurh") == 0)
        cast = "(uint16_t)";
      else if (std::strcmp(m, "ldrsw") == 0)
        cast = "(int32_t)";
      // mem 可能是 [xn] 或 [xn, #imm] 已完整在 parts[1]
      std::snprintf(insn.pseudo, sizeof(insn.pseudo), "%s = %s*%s;", parts[0],
                    cast, parts[1]);
    } else
      fallback();
    return;
  }
  if (std::strncmp(m, "str", 3) == 0 || std::strncmp(m, "stur", 4) == 0) {
    char parts[4][64]{};
    int pn = split_ops(o, parts, 4);
    if (pn >= 2)
      std::snprintf(insn.pseudo, sizeof(insn.pseudo), "*%s = %s;", parts[1],
                    parts[0]);
    else
      fallback();
    return;
  }
  if (std::strcmp(m, "stp") == 0) {
    char parts[4][64]{};
    int pn = split_ops(o, parts, 4);
    if (pn >= 3)
      std::snprintf(insn.pseudo, sizeof(insn.pseudo), "*%s = {%s, %s};",
                    parts[2], parts[0], parts[1]);
    else
      std::snprintf(insn.pseudo, sizeof(insn.pseudo), "store_pair(%s);", o);
    return;
  }
  if (std::strcmp(m, "ldp") == 0) {
    char parts[4][64]{};
    int pn = split_ops(o, parts, 4);
    if (pn >= 3)
      std::snprintf(insn.pseudo, sizeof(insn.pseudo), "{%s, %s} = *%s;",
                    parts[0], parts[1], parts[2]);
    else
      std::snprintf(insn.pseudo, sizeof(insn.pseudo), "load_pair(%s);", o);
    return;
  }
  if (std::strcmp(m, "cmp") == 0 || std::strcmp(m, "cmn") == 0 ||
      std::strcmp(m, "tst") == 0) {
    char a[24]{}, b[40]{};
    if (std::sscanf(o, "%23[^,], %39s", a, b) >= 2) {
      if (std::strcmp(m, "cmp") == 0)
        std::snprintf(insn.pseudo, sizeof(insn.pseudo),
                      "flags = (%s - %s); // NZCV", a, b);
      else if (std::strcmp(m, "cmn") == 0)
        std::snprintf(insn.pseudo, sizeof(insn.pseudo),
                      "flags = (%s + %s); // NZCV", a, b);
      else
        std::snprintf(insn.pseudo, sizeof(insn.pseudo),
                      "flags = (%s & %s); // NZCV", a, b);
    } else
      fallback();
    return;
  }
  if (std::strcmp(m, "adrp") == 0 || std::strcmp(m, "adr") == 0) {
    char d[24]{}, t[48]{};
    if (std::sscanf(o, "%23[^,], %47s", d, t) >= 2)
      std::snprintf(insn.pseudo, sizeof(insn.pseudo),
                    "%s = %s(%s);", d,
                    std::strcmp(m, "adrp") == 0 ? "page_addr" : "pc_rel", t);
    else
      fallback();
    return;
  }
  if (std::strcmp(m, "svc") == 0) {
    std::snprintf(insn.pseudo, sizeof(insn.pseudo),
                  "syscall(%s); // x0-x5 args, ret x0", o[0] ? o : "0");
    return;
  }
  if (std::strcmp(m, "mrs") == 0 || std::strcmp(m, "msr") == 0) {
    std::snprintf(insn.pseudo, sizeof(insn.pseudo), "sysreg_%s(%s);", m, o);
    return;
  }
  // 移位类
  if (std::strcmp(m, "lsl") == 0 || std::strcmp(m, "lsr") == 0 ||
      std::strcmp(m, "asr") == 0 || std::strcmp(m, "ror") == 0) {
    char d[24]{}, a[24]{}, b[24]{};
    if (std::sscanf(o, "%23[^,], %23[^,], %23s", d, a, b) >= 3) {
      const char* op = std::strcmp(m, "lsl") == 0   ? "<<"
                       : std::strcmp(m, "lsr") == 0 ? ">>"
                       : std::strcmp(m, "asr") == 0 ? ">>>"
                                                    : "ror";
      std::snprintf(insn.pseudo, sizeof(insn.pseudo), "%s = %s %s %s;", d, a, op,
                    b);
    } else
      fallback();
    return;
  }
  fallback();
}

InsnCat classify_mnem(const char* mnem) {
  if (!mnem || !mnem[0]) return InsnCat::Other;
  if (is_noise_mnem(mnem)) return InsnCat::Noise;
  if (std::strcmp(mnem, "ret") == 0 || std::strcmp(mnem, "br") == 0 ||
      std::strcmp(mnem, "blr") == 0 || std::strcmp(mnem, "b") == 0 ||
      std::strcmp(mnem, "bl") == 0 || std::strcmp(mnem, "cbz") == 0 ||
      std::strcmp(mnem, "cbnz") == 0 || std::strcmp(mnem, "tbz") == 0 ||
      std::strcmp(mnem, "tbnz") == 0 || std::strncmp(mnem, "b.", 2) == 0)
    return InsnCat::Branch;
  if (std::strncmp(mnem, "ldr", 3) == 0 || std::strcmp(mnem, "ldp") == 0 ||
      std::strncmp(mnem, "ldu", 3) == 0 || std::strcmp(mnem, "prfm") == 0)
    return InsnCat::Load;
  if (std::strncmp(mnem, "str", 3) == 0 || std::strcmp(mnem, "stp") == 0 ||
      std::strncmp(mnem, "stu", 3) == 0)
    return InsnCat::Store;
  if (std::strncmp(mnem, "mov", 3) == 0 || std::strcmp(mnem, "mvn") == 0)
    return InsnCat::Mov;
  if (std::strcmp(mnem, "cmp") == 0 || std::strcmp(mnem, "cmn") == 0 ||
      std::strcmp(mnem, "tst") == 0 || std::strcmp(mnem, "ccmp") == 0 ||
      std::strcmp(mnem, "ccmn") == 0 || std::strcmp(mnem, "fcmp") == 0)
    return InsnCat::Compare;
  if (std::strcmp(mnem, "svc") == 0 || std::strcmp(mnem, "mrs") == 0 ||
      std::strcmp(mnem, "msr") == 0 || std::strcmp(mnem, "sys") == 0 ||
      std::strcmp(mnem, "dsb") == 0 || std::strcmp(mnem, "dmb") == 0 ||
      std::strcmp(mnem, "isb") == 0)
    return InsnCat::System;
  if (mnem[0] == 'f' || std::strcmp(mnem, "scvtf") == 0 ||
      std::strcmp(mnem, "fcvtzs") == 0)
    return InsnCat::Float;
  if (std::strcmp(mnem, "add") == 0 || std::strcmp(mnem, "sub") == 0 ||
      std::strcmp(mnem, "adds") == 0 || std::strcmp(mnem, "subs") == 0 ||
      std::strcmp(mnem, "and") == 0 || std::strcmp(mnem, "orr") == 0 ||
      std::strcmp(mnem, "eor") == 0 || std::strcmp(mnem, "mul") == 0 ||
      std::strcmp(mnem, "adrp") == 0 || std::strcmp(mnem, "adr") == 0 ||
      std::strcmp(mnem, "udiv") == 0 || std::strcmp(mnem, "sdiv") == 0 ||
      std::strcmp(mnem, "lsl") == 0 || std::strcmp(mnem, "lsr") == 0 ||
      std::strcmp(mnem, "asr") == 0 || std::strcmp(mnem, "ror") == 0 ||
      std::strcmp(mnem, "neg") == 0 || std::strcmp(mnem, "clz") == 0)
    return InsnCat::Alu;
  if (std::strcmp(mnem, ".word") == 0) return InsnCat::Noise;
  return InsnCat::Other;
}

bool is_noise_mnem(const char* mnem) {
  if (!mnem) return true;
  // 无实际语义 / 指针认证 / 提示 / 填充
  static const char* kNoise[] = {
      "nop",   "hint",  "yield", "wfe",   "wfi",   "sev",   "sevl",
      "bti",   "paciasp", "autiasp", "pacibsp", "autibsp",
      "xpaclri", "xpaci", "xpacd", "autia1716", "pacia1716",
      "csdb",  "esb",   "tsb",   "psb",   "dgh",
      ".word", "udf",   "brk",
      nullptr};
  for (int i = 0; kNoise[i]; ++i)
    if (std::strcmp(mnem, kNoise[i]) == 0) return true;
  // pac*/aut* 前缀
  if (std::strncmp(mnem, "pac", 3) == 0 || std::strncmp(mnem, "aut", 3) == 0)
    return true;
  if (std::strncmp(mnem, "bti", 3) == 0) return true;
  return false;
}

std::vector<Insn> disasm_arm64(uintptr_t base, const uint8_t* code, size_t len,
                               const DisasmOptions& opt) {
  std::vector<Insn> out;
  size_t n = len / 4;
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    uint32_t w;
    std::memcpy(&w, code + i * 4, 4);
    Insn insn;
    decode_one(insn, base + i * 4, w);
    insn.cat = classify_mnem(insn.mnem);
    insn.is_noise = is_noise_mnem(insn.mnem);
    if (opt.filter_noise && insn.is_noise) continue;
    insn_to_pseudo(insn);
    char full[160];
    std::snprintf(full, sizeof(full), "%llX:  %08X  %-6s %s",
                  (unsigned long long)insn.addr, insn.raw, insn.mnem, insn.ops);
    std::snprintf(insn.text, sizeof(insn.text), "%s", full);
    out.push_back(insn);
  }
  enrich_insns(out, opt);
  return out;
}

std::vector<Insn> disasm_at(uintptr_t addr, int count, const DisasmOptions& opt) {
  if (count < 1) count = 1;
  if (count > 256) count = 256;
  // 多读一些以便过滤后仍够条数
  int read_n = opt.filter_noise ? count * 4 : count;
  if (read_n > 512) read_n = 512;
  std::vector<uint8_t> buf((size_t)read_n * 4);
  if (!read_mem(addr, buf.data(), buf.size())) return {};
  DisasmOptions o = opt;
  o.max_insns = count;
  auto all = disasm_arm64(addr, buf.data(), buf.size(), o);
  // 过滤后为空（如 ELF 头/全是 .word·pac）：回退显示未过滤结果，避免 UI 空白
  if (all.empty() && opt.filter_noise) {
    o.filter_noise = false;
    all = disasm_arm64(addr, buf.data(), buf.size(), o);
  }
  if ((int)all.size() > count) all.resize((size_t)count);
  return all;
}

bool find_func_range(uintptr_t around, uintptr_t& out_start, uintptr_t& out_end) {
  // 向前最多 0x800 字节找 STP x29, x30 序言；向后找 RET
  const size_t back = 0x800;
  const size_t forth = 0x1000;
  uintptr_t start = around > back ? around - back : (around & ~0xFull);
  start &= ~3ull;

  std::vector<uint8_t> buf(back + forth + 16);
  if (!read_mem(start, buf.data(), buf.size())) {
    out_start = around & ~3ull;
    out_end = out_start + 64;
    return false;
  }

  uintptr_t found_start = around & ~3ull;
  // 从 around 向前扫
  for (uintptr_t a = around & ~3ull; a > start + 4; a -= 4) {
    size_t off = a - start;
    if (off + 4 > buf.size()) break;
    uint32_t w;
    std::memcpy(&w, buf.data() + off, 4);
    // STP X29, X30, [SPn, #imm]! 常见 0xA9xx7BFD
    if ((w & 0xFFC07FFF) == 0xA9007BFD || (w & 0xFFC003E0) == 0xA90003E0) {
      // 粗匹配 STP with fp/lr
      if (((w >> 10) & 0x1F) == 30 || ((w) & 0x1F) == 29) {
        found_start = a;
        break;
      }
    }
    // paciasp / bti
    if (w == 0xD503233F || w == 0xD503245F) {
      found_start = a;
      break;
    }
  }

  uintptr_t found_end = found_start + 64;
  for (uintptr_t a = around & ~3ull; a < start + buf.size() - 4; a += 4) {
    size_t off = a - start;
    uint32_t w;
    std::memcpy(&w, buf.data() + off, 4);
    if ((w & 0xFFFFFC1F) == 0xD65F0000) {  // RET
      found_end = a + 4;
      if (a >= around) break;
    }
    if (a - found_start > forth) break;
  }

  out_start = found_start;
  out_end = found_end > found_start ? found_end : found_start + 64;
  return true;
}

std::vector<Insn> disasm_function(uintptr_t around, int max_insns,
                                  const DisasmOptions& opt) {
  uintptr_t s = 0, e = 0;
  find_func_range(around, s, e);
  size_t bytes = e - s;
  int want = opt.filter_noise ? max_insns * 4 : max_insns;
  if (bytes > (size_t)want * 4) bytes = (size_t)want * 4;
  if (bytes < 16) bytes = 16;
  std::vector<uint8_t> buf(bytes);
  DisasmOptions o = opt;
  o.max_insns = max_insns;
  if (!read_mem(s, buf.data(), buf.size()))
    return disasm_at(around, max_insns > 0 ? max_insns : 32, o);
  auto all = disasm_arm64(s, buf.data(), buf.size(), o);
  if (all.empty() && opt.filter_noise) {
    o.filter_noise = false;
    all = disasm_arm64(s, buf.data(), buf.size(), o);
  }
  if ((int)all.size() > max_insns) all.resize((size_t)max_insns);
  return all;
}

std::vector<Xref> build_xrefs(const std::vector<Insn>& insns) {
  std::vector<Xref> xs;
  xs.reserve(insns.size() / 4 + 4);
  for (auto& i : insns) {
    if (!i.target) continue;
    if (!i.is_branch && std::strcmp(i.mnem, "adrp") != 0 &&
        std::strcmp(i.mnem, "adr") != 0 &&
        !(std::strcmp(i.mnem, "ldr") == 0 && i.target))
      continue;
    Xref x;
    x.from = i.addr;
    x.to = i.target;
    std::snprintf(x.mnem, sizeof(x.mnem), "%s", i.mnem);
    if (i.is_call)
      std::snprintf(x.kind, sizeof(x.kind), "call");
    else if (i.is_branch)
      std::snprintf(x.kind, sizeof(x.kind), "branch");
    else
      std::snprintf(x.kind, sizeof(x.kind), "lit");
    xs.push_back(x);
  }
  return xs;
}

void enrich_insns(std::vector<Insn>& insns, const DisasmOptions& opt) {
  if (insns.empty()) return;
  if (opt.resolve_symbols && sym_count() == 0) sym_refresh();

  // 1) 统计本流内被引用的地址
  std::vector<uintptr_t> targets;
  targets.reserve(insns.size());
  for (auto& i : insns) {
    if (i.target && (i.is_branch || i.is_call)) targets.push_back(i.target);
  }

  for (auto& i : insns) {
    i.xref_in = 0;
    i.is_label = false;
    i.label[0] = 0;
    i.target_name[0] = 0;
    for (auto t : targets) {
      if (t == i.addr) i.xref_in++;
    }
    if (i.xref_in > 0) {
      i.is_label = true;
      // 优先符号名
      if (opt.resolve_symbols) {
        const char* sn = sym_name_at(i.addr);
        if (sn && sn[0])
          std::snprintf(i.label, sizeof(i.label), "%s", sn);
        else
          std::snprintf(i.label, sizeof(i.label), "loc_%llX",
                        (unsigned long long)i.addr);
      } else {
        std::snprintf(i.label, sizeof(i.label), "loc_%llX",
                      (unsigned long long)i.addr);
      }
    }
    if (opt.resolve_symbols && i.target) {
      const char* tn = sym_name_at(i.target);
      if (tn && tn[0]) {
        std::snprintf(i.target_name, sizeof(i.target_name), "%s", tn);
      } else {
        // 是否为本流内标签
        for (auto& j : insns) {
          if (j.addr == i.target && j.is_label && j.label[0]) {
            std::snprintf(i.target_name, sizeof(i.target_name), "%s", j.label);
            break;
          }
        }
        if (!i.target_name[0] && (i.is_branch || i.is_call))
          std::snprintf(i.target_name, sizeof(i.target_name), "loc_%llX",
                        (unsigned long long)i.target);
      }
      // 回写 ops 里的纯地址为符号（call/branch）
      if (i.target_name[0] && (i.is_call || (i.is_branch && !i.is_cond &&
                                             std::strcmp(i.mnem, "b") == 0))) {
        if (std::strcmp(i.mnem, "bl") == 0 || std::strcmp(i.mnem, "b") == 0) {
          std::snprintf(i.ops, sizeof(i.ops), "%s", i.target_name);
          insn_to_pseudo(i);
        }
      } else if (i.target_name[0] && i.is_cond) {
        // 条件跳：保留寄存器，替换地址尾部
        // 简化：pseudo 里用 target_name
        char* p = std::strstr(i.pseudo, "goto ");
        if (p) {
          char buf[192];
          std::snprintf(buf, sizeof(buf), "%.*sgoto %s;",
                        (int)(p - i.pseudo), i.pseudo, i.target_name);
          std::snprintf(i.pseudo, sizeof(i.pseudo), "%s", buf);
        }
      }
    }
    // 本址若有符号且无 label，作为函数入口标注
    if (opt.resolve_symbols && !i.label[0]) {
      const char* sn = sym_name_at(i.addr);
      if (sn && sn[0] && i.addr == insns.front().addr) {
        i.is_label = true;
        std::snprintf(i.label, sizeof(i.label), "%s", sn);
      }
    }
    char full[160];
    if (i.label[0])
      std::snprintf(full, sizeof(full), "%llX:  %08X  %-6s %s  ; %s xref=%d",
                    (unsigned long long)i.addr, i.raw, i.mnem, i.ops, i.label,
                    i.xref_in);
    else if (i.target_name[0])
      std::snprintf(full, sizeof(full), "%llX:  %08X  %-6s %s  -> %s",
                    (unsigned long long)i.addr, i.raw, i.mnem, i.ops,
                    i.target_name);
    else
      std::snprintf(full, sizeof(full), "%llX:  %08X  %-6s %s",
                    (unsigned long long)i.addr, i.raw, i.mnem, i.ops);
    std::snprintf(i.text, sizeof(i.text), "%s", full);
  }
}

std::string insns_to_pseudo_c(const std::vector<Insn>& insns,
                              uintptr_t func_addr) {
  // 默认走 CFG 版；失败时仍有内容
  return insns_to_cfg_pseudo_c(insns, func_addr);
}

std::string insns_to_cfg_pseudo_c(const std::vector<Insn>& insns,
                                  uintptr_t func_addr) {
  std::string out;
  char head[320];
  const char* fname = sym_name_at(func_addr);
  if (fname && fname[0])
    std::snprintf(head, sizeof(head),
                  "// CFG pseudo-C  @ 0x%llX  %s\n"
                  "// ABI: x0-x7 args/ret · x29=fp · x30=lr\n"
                  "// 结构：标签 + if/goto（轻量控制流恢复）\n"
                  "void %s(/* x0-x7 */) {\n",
                  (unsigned long long)func_addr, fname, fname);
  else
    std::snprintf(head, sizeof(head),
                  "// CFG pseudo-C  @ 0x%llX\n"
                  "// ABI: x0-x7 args/ret · x29=fp · x30=lr\n"
                  "void sub_%llX(/* x0-x7 */) {\n",
                  (unsigned long long)func_addr,
                  (unsigned long long)func_addr);
  out += head;

  // xrefs 注释
  auto xs = build_xrefs(insns);
  if (!xs.empty()) {
    out += "  // xrefs in stream:\n";
    int shown = 0;
    for (auto& x : xs) {
      if (shown >= 12) {
        out += "  //   ...\n";
        break;
      }
      char line[120];
      std::snprintf(line, sizeof(line), "  //   0x%llX -%s-> 0x%llX  (%s)\n",
                    (unsigned long long)x.from, x.kind,
                    (unsigned long long)x.to, x.mnem);
      out += line;
      shown++;
    }
  }

  // 地址 → 是否为标签
  auto find_idx = [&](uintptr_t a) -> int {
    for (int i = 0; i < (int)insns.size(); ++i)
      if (insns[(size_t)i].addr == a) return i;
    return -1;
  };

  int n = 0;
  for (size_t i = 0; i < insns.size(); ++i) {
    Insn tmp = insns[i];
    char line[300];
    // 即使是 nop 等 noise，若是跳转目标也要保留标签
    if (tmp.is_label && tmp.label[0]) {
      std::snprintf(line, sizeof(line), "\n%s:\n", tmp.label);
      out += line;
    }
    if (tmp.is_noise) continue;
    if (!tmp.pseudo[0]) insn_to_pseudo(tmp);

    // 无条件 b 且目标在流内 → goto label
    if (std::strcmp(tmp.mnem, "b") == 0 && tmp.target) {
      const char* lab =
          tmp.target_name[0] ? tmp.target_name : nullptr;
      char loc[40];
      if (!lab) {
        std::snprintf(loc, sizeof(loc), "loc_%llX",
                      (unsigned long long)tmp.target);
        lab = loc;
      }
      std::snprintf(line, sizeof(line), "  goto %s;  // %llX\n", lab,
                    (unsigned long long)tmp.addr);
      out += line;
      n++;
      continue;
    }

    // 条件分支：if (cond) goto lab;  — 若 fallthrough 短块可包成 if
    if (tmp.is_cond && tmp.target) {
      int ti = find_idx(tmp.target);
      const char* lab =
          tmp.target_name[0] ? tmp.target_name : nullptr;
      char loc[40];
      if (!lab) {
        std::snprintf(loc, sizeof(loc), "loc_%llX",
                      (unsigned long long)tmp.target);
        lab = loc;
      }
      // 提取条件表达式（来自 pseudo 的 if (...) 部分）
      char cond[96] = "cond";
      if (std::strncmp(tmp.pseudo, "if (", 4) == 0) {
        const char* p = tmp.pseudo + 4;
        const char* q = std::strstr(p, ") goto");
        if (q && (size_t)(q - p) < sizeof(cond)) {
          std::memcpy(cond, p, (size_t)(q - p));
          cond[q - p] = 0;
        }
      }
      // 前向跳 + 目标在流内：写成 if (!cond) { fallthrough... } 过重
      // 轻量：统一 if (cond) goto
      bool backward = tmp.target <= tmp.addr;
      if (backward) {
        std::snprintf(line, sizeof(line),
                      "  if (%s) goto %s;  // loop %llX\n", cond, lab,
                      (unsigned long long)tmp.addr);
      } else if (ti >= 0) {
        std::snprintf(line, sizeof(line),
                      "  if (%s) goto %s;  // %llX\n", cond, lab,
                      (unsigned long long)tmp.addr);
      } else {
        std::snprintf(line, sizeof(line),
                      "  if (%s) goto %s;  // ext %llX\n", cond, lab,
                      (unsigned long long)tmp.addr);
      }
      out += line;
      n++;
      continue;
    }

    // 调用
    if (tmp.is_call) {
      const char* tn =
          tmp.target_name[0] ? tmp.target_name
                             : (tmp.ops[0] ? tmp.ops : "fn");
      std::snprintf(line, sizeof(line),
                    "  x0 = %s(/* x0-x7 */);  // %llX\n", tn,
                    (unsigned long long)tmp.addr);
      out += line;
      n++;
      continue;
    }

    if (tmp.pseudo[0]) {
      std::snprintf(line, sizeof(line), "  %-48s // %llX %s\n", tmp.pseudo,
                    (unsigned long long)tmp.addr, tmp.mnem);
    } else {
      std::snprintf(line, sizeof(line), "  asm(\"%s %s\"); // %llX\n",
                    tmp.mnem, tmp.ops, (unsigned long long)tmp.addr);
    }
    out += line;
    n++;
  }

  if (n == 0)
    out += "  /* empty */\n  return;\n";
  else
    out += "\n  // end CFG body\n";
  out += "}\n";
  return out;
}

}  // namespace mem

