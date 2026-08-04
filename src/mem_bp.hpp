#pragma once
/**
 * 硬件断点 / 观察点 / 动态调试深度
 * - perf 硬件断点 + ptrace 回退
 * - 软断点 SIGTRAP 闭环、条件断点
 * - 步入/步过/步出、线程切换
 * - 通用 + 浮点寄存器、指令 Trace
 * - 代码注入 / 远程 call
 */
#include <cstdint>
#include <string>
#include <vector>

namespace mem {

enum class BpType : int {
  Exec = 0,
  WatchW,
  WatchR,
  WatchRW,
};

enum class BpBackend : int {
  None = 0,
  Perf,
  Ptrace,
};

struct BpInfo {
  int id = -1;
  int slot = -1;
  BpType type = BpType::Exec;
  uintptr_t addr = 0;
  int size = 4;
  bool active = false;
  bool enabled = true;
  uint64_t hit_count = 0;
  char note[48]{};
  char cond[48]{};  // 条件：空=总是；如 x0==1 / x1!=0
};

struct BpHit {
  bool valid = false;
  int bp_id = -1;
  uintptr_t pc = 0;
  uintptr_t lr = 0;
  uintptr_t sp = 0;
  uintptr_t fp = 0;
  uintptr_t watch_addr = 0;
  BpType type = BpType::Exec;
  uint64_t hit_index = 0;
  char msg[192]{};
  bool is_soft = false;
  int tid = 0;
};

// ── 生命周期 ──────────────────────────────────────────────
bool bp_init();
void bp_shutdown();
bool bp_ready();
BpBackend bp_backend();
const char* bp_backend_name();

bool bp_attach_ptrace();
void bp_detach_ptrace();
bool bp_ptrace_active();

// ── 断点管理 ──────────────────────────────────────────────
int bp_set(uintptr_t addr, BpType type, int size = 4);
/** 带条件的硬件/执行断点 */
int bp_set_cond(uintptr_t addr, BpType type, int size, const char* cond);
bool bp_set_condition(int id, const char* cond);
bool bp_clear(int id);
void bp_clear_all();
bool bp_enable(int id, bool on);
std::vector<BpInfo> bp_list();
int bp_count();
int bp_max_slots();

bool bp_arm_and_continue();
bool bp_poll(BpHit& hit);
bool bp_continue();
bool bp_is_stopped();

std::vector<BpHit> bp_hit_log(size_t max_n = 32);
void bp_clear_hit_log();
const char* bp_status();
const char* bp_type_name(BpType t);

// ── 寄存器 ────────────────────────────────────────────────
struct Regs {
  uint64_t x[31]{};
  uint64_t sp = 0;
  uint64_t pc = 0;
  uint64_t pstate = 0;
  bool valid = false;
};

/** 浮点 / SIMD：v0–v31 各 128bit（存为 2×u64）+ d0–d31 双精度视图 */
struct FpRegs {
  uint64_t v[32][2]{};  // low, high
  double d[32]{};       // 低 64 位解释为 double
  bool valid = false;
};

struct ThreadInfo {
  int tid = 0;
  char name[48]{};
  char state[8]{};
};

struct ModuleInfo {
  uintptr_t start = 0;
  uintptr_t end = 0;
  char perms[8]{};
  std::string path;
};

struct SoftBp {
  int id = -1;
  uintptr_t addr = 0;
  uint32_t orig = 0;
  bool active = false;
  char cond[48]{};
  bool oneshot = false;
};

struct TraceEntry {
  uintptr_t pc = 0;
  uint32_t raw = 0;
  char text[64]{};
};

// ── 暂停 / 单步 / 步过 / 步出 ─────────────────────────────
bool dbg_pause();
bool dbg_resume();
bool dbg_step();       // 步入（单指令）
bool dbg_step_over();  // 步过（BL 则断在下一条）
bool dbg_step_out();   // 步出（断在 LR）
bool dbg_is_paused();

/** 每帧轮询：SIGTRAP / 软断点命中 / 临时断点 */
bool dbg_poll_stop(BpHit& hit);

// ── 线程 ──────────────────────────────────────────────────
bool dbg_set_tid(int tid);  // 0 = 主进程 pid
int dbg_get_tid();
bool dbg_attach_thread(int tid);

// ── 寄存器读写 ────────────────────────────────────────────
bool dbg_regs_read(Regs& out);
bool dbg_regs_write(const Regs& in);
bool dbg_reg_set(const char* name, uint64_t val);
bool dbg_fp_regs_read(FpRegs& out);

// ── 软断点 ────────────────────────────────────────────────
int soft_bp_set(uintptr_t addr);
int soft_bp_set_cond(uintptr_t addr, const char* cond, bool oneshot = false);
bool soft_bp_clear(int id);
void soft_bp_clear_all();
std::vector<SoftBp> soft_bp_list();

// ── 补丁 ──────────────────────────────────────────────────
bool patch_nop(uintptr_t addr, int count = 1);
bool patch_bytes(uintptr_t addr, const uint8_t* data, size_t len);
bool patch_hex(uintptr_t addr, const char* hex);

// ── Trace ─────────────────────────────────────────────────
/** 单步 max_n 条，记录 PC（需已 pause 或会先 pause） */
bool dbg_trace(int max_n, std::vector<TraceEntry>& out);

// ── 注入 / 远程 call ──────────────────────────────────────
/** 向可写可执行区域写入代码；失败返回 false */
bool inject_code(uintptr_t addr, const uint8_t* code, size_t len);
/**
 * 远程调用：设置 x0–x7 / PC=fn / LR=断点陷阱，运行至返回。
 * 成功后 *ret = x0，并恢复原寄存器。
 */
bool remote_call(uintptr_t fn, const uint64_t* args, int nargs, uint64_t* ret);
/**
 * 简易 so 注入：remote_call(dlopen, path, RTLD_NOW)。
 * dlopen_addr==0 时自动从符号表解析 dlopen / __loader_dlopen。
 */
bool inject_so(uintptr_t dlopen_addr, const char* path, uint64_t* handle_out);
/** 自动解析 dlopen 地址 */
bool find_dlopen(uintptr_t& out_addr);

// ── 其它 ──────────────────────────────────────────────────
std::vector<uintptr_t> dbg_stack_trace(int max_frames = 16);
std::vector<ThreadInfo> list_threads();
std::vector<ModuleInfo> list_modules(const char* filter = nullptr);
std::string module_of(uintptr_t addr);

/** 条件表达式求值（x0==1 / x1!=0 / x2>5 / pc==0x...） */
bool eval_condition(const char* cond, const Regs& r);

}  // namespace mem
