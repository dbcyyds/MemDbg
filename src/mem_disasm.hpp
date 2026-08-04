#pragma once
/**
 * ARM64 反汇编 + 语法着色 + xrefs + 符号 + CFG 伪 C + 汇编写回
 */
#include <cstdint>
#include <string>
#include <vector>

namespace mem {

/** 指令类别（着色 / 过滤） */
enum class InsnCat : int {
  Alu = 0,    // add/sub/and/orr…
  Mov,        // mov/movz/movk
  Load,       // ldr/ldp/ldrb…
  Store,      // str/stp…
  Branch,     // b/bl/br/cbz/ret
  Compare,    // cmp/cmn/tst
  System,     // svc/mrs/msr
  Float,      // fmov/fadd/fmul…
  Noise,      // nop/hint/pac/bti/cfi/.word 无用
  Other,
};

struct Insn {
  uintptr_t addr = 0;
  uint32_t raw = 0;
  char mnem[24]{};
  char ops[96]{};
  char text[160]{};
  char pseudo[192]{};
  bool is_branch = false;
  bool is_ret = false;
  bool is_call = false;
  bool is_noise = false;
  bool is_cond = false;     // 条件分支
  bool is_label = false;    // 本址是跳转目标
  InsnCat cat = InsnCat::Other;
  uintptr_t target = 0;
  int xref_in = 0;          // 有多少条指令跳到这里
  char label[40]{};         // loc_xxxx 或符号
  char target_name[64]{};   // 目标符号 / loc
};

struct DisasmOptions {
  bool filter_noise = true;
  int max_insns = 120;
  bool resolve_symbols = true;  // bl/b 目标解析符号
  bool build_xrefs = true;      // 标注标签与 xref 计数
};

struct Xref {
  uintptr_t from = 0;
  uintptr_t to = 0;
  char kind[12]{};  // call / branch / lit
  char mnem[24]{};
};

struct SymInfo {
  uintptr_t addr = 0;
  size_t size = 0;
  char name[96]{};
  char module[80]{};
};

// ── 反汇编 ────────────────────────────────────────────────
std::vector<Insn> disasm_arm64(uintptr_t base, const uint8_t* code, size_t len,
                               const DisasmOptions& opt = {});

std::vector<Insn> disasm_at(uintptr_t addr, int count = 48,
                            const DisasmOptions& opt = {});

bool find_func_range(uintptr_t around, uintptr_t& out_start, uintptr_t& out_end);

std::vector<Insn> disasm_function(uintptr_t around, int max_insns = 120,
                                  const DisasmOptions& opt = {});

/** 逐条伪 C 模板（旧） */
std::string insns_to_pseudo_c(const std::vector<Insn>& insns, uintptr_t func_addr);

/** CFG 结构恢复伪 C：if/goto/label，优于纯逐条模板 */
std::string insns_to_cfg_pseudo_c(const std::vector<Insn>& insns,
                                  uintptr_t func_addr);

void insn_to_pseudo(Insn& insn);

/** 对已反汇编结果：标标签、解析符号、填 xref 计数 */
void enrich_insns(std::vector<Insn>& insns, const DisasmOptions& opt = {});

/** 从指令流构建交叉引用 */
std::vector<Xref> build_xrefs(const std::vector<Insn>& insns);

// ── 符号 ──────────────────────────────────────────────────
/** 从当前附加进程 maps 刷新 ELF 导出/动态符号（缓存） */
int sym_refresh();
/** 精确或落在函数范围内的符号名；无则返回空串 */
const char* sym_name_at(uintptr_t addr);
/** 最佳匹配符号信息 */
bool sym_lookup(uintptr_t addr, SymInfo& out);
/** 最近符号（addr 附近） */
bool sym_nearest(uintptr_t addr, SymInfo& out, size_t max_dist = 0x10000);
size_t sym_count();
/** 按符号名查找（精确或后缀匹配）；优先 libc/libdl/linker */
bool sym_find_by_name(const char* name, uintptr_t& out_addr,
                      char* mod_out = nullptr, size_t mod_cap = 0);

// ── 分类 ──────────────────────────────────────────────────
InsnCat classify_mnem(const char* mnem);
bool is_noise_mnem(const char* mnem);

// ── 汇编器 / 写回 ─────────────────────────────────────────
/** 单条 ARM64 汇编 → 机器码。成功返回 true */
bool assemble_line(const char* line, uint32_t& out_word, char* err = nullptr,
                   size_t err_cap = 0);
/** 多行（; 或换行分隔） */
bool assemble_text(const char* text, std::vector<uint32_t>& out,
                   char* err = nullptr, size_t err_cap = 0);
/** 汇编并写回进程内存（覆盖等长指令） */
bool patch_asm(uintptr_t addr, const char* asm_text);
/** 将一行汇编编码为 hex 字符串（调试用） */
bool assemble_to_hex(const char* line, char* hex_out, size_t hex_cap);

}  // namespace mem
