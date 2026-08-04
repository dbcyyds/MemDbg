#include "mem_bp.hpp"
#include "mem_core.hpp"
#include "mem_disasm.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <deque>
#include <dirent.h>
#include <mutex>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <poll.h>
#include <fcntl.h>
#include <vector>

#ifndef __WALL
#define __WALL 0x40000000
#endif

namespace mem {
// 前向声明（实现顺序交叉）
bool dbg_resume();
void rearm_all_soft_bps_locked();
int focus_tid();
bool soft_bp_rearm_step_locked(bool keep_paused);
}  // namespace mem

// ptrace 回退
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <linux/elf.h>

#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif
#ifndef NT_ARM_HW_BREAK
#define NT_ARM_HW_BREAK 0x402
#endif
#ifndef NT_ARM_HW_WATCH
#define NT_ARM_HW_WATCH 0x403
#endif
#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_INTERRUPT
#define PTRACE_INTERRUPT 0x4207
#endif

#ifndef PERF_FLAG_FD_CLOEXEC
#define PERF_FLAG_FD_CLOEXEC (1UL << 3)
#endif

namespace mem {
namespace {

std::mutex g_mu;
BpBackend g_backend = BpBackend::None;
char g_status[160] = "断点未初始化";
int g_next_id = 1;
bool g_stopped = false;  // ptrace 暂停
bool g_dbg_paused = false;
int g_focus_tid = 0;  // 0 = 主 pid
std::deque<BpHit> g_hit_log;
constexpr size_t kMaxLog = 64;

// ARM64 user_pt_regs（提前声明，供 perf 条件断点使用）
struct user_pt_regs {
  uint64_t regs[31];
  uint64_t sp;
  uint64_t pc;
  uint64_t pstate;
};

std::vector<SoftBp> g_soft;
int g_soft_next = 1;

bool eval_condition_impl(const char* cond, const Regs& r) {
  if (!cond || !cond[0]) return true;
  char cbuf[64]{};
  size_t w = 0;
  for (const char* p = cond; *p && w + 1 < sizeof(cbuf); ++p)
    if (*p != ' ' && *p != '\t') cbuf[w++] = *p;
  cbuf[w] = 0;
  char reg[8]{};
  char op[4]{};
  char val[32]{};
  const char* p = cbuf;
  int ri = 0;
  while (*p && ri < 7 &&
         ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9')))
    reg[ri++] = *p++;
  reg[ri] = 0;
  if (p[0] == '=' && p[1] == '=') {
    std::snprintf(op, sizeof(op), "==");
    p += 2;
  } else if (p[0] == '!' && p[1] == '=') {
    std::snprintf(op, sizeof(op), "!=");
    p += 2;
  } else if (p[0] == '>' && p[1] == '=') {
    std::snprintf(op, sizeof(op), ">=");
    p += 2;
  } else if (p[0] == '<' && p[1] == '=') {
    std::snprintf(op, sizeof(op), "<=");
    p += 2;
  } else if (*p == '>' || *p == '<') {
    op[0] = *p++;
    op[1] = 0;
  } else
    return true;
  std::snprintf(val, sizeof(val), "%s", p);
  uint64_t rv = 0;
  if (std::strcmp(reg, "pc") == 0 || std::strcmp(reg, "PC") == 0)
    rv = r.pc;
  else if (std::strcmp(reg, "sp") == 0 || std::strcmp(reg, "SP") == 0)
    rv = r.sp;
  else if (std::strcmp(reg, "lr") == 0 || std::strcmp(reg, "LR") == 0)
    rv = r.x[30];
  else if (std::strcmp(reg, "fp") == 0 || std::strcmp(reg, "FP") == 0)
    rv = r.x[29];
  else if ((reg[0] == 'x' || reg[0] == 'X' || reg[0] == 'w' || reg[0] == 'W') &&
           reg[1]) {
    int n = std::atoi(reg + 1);
    if (n >= 0 && n <= 30) rv = r.x[n];
  } else
    return true;
  uint64_t want = std::strtoull(val, nullptr, 0);
  if (std::strcmp(op, "==") == 0) return rv == want;
  if (std::strcmp(op, "!=") == 0) return rv != want;
  if (std::strcmp(op, ">") == 0) return rv > want;
  if (std::strcmp(op, "<") == 0) return rv < want;
  if (std::strcmp(op, ">=") == 0) return rv >= want;
  if (std::strcmp(op, "<=") == 0) return rv <= want;
  return true;
}

// ── perf 断点 ─────────────────────────────────────────────
struct PerfBp {
  BpInfo info;
  int fd = -1;
  void* mmap_base = nullptr;
  size_t mmap_size = 0;
  uint64_t last_count = 0;
};

std::vector<PerfBp> g_perf;
long g_page = 4096;

// ── ptrace 回退 ───────────────────────────────────────────
// ARM64 user_hwdebug_state：内核只接受「实际槽位数」大小的 iovec
// （传 16 槽完整结构会 ENOSPC）
struct user_hwdebug_state {
  uint32_t dbg_info;
  uint32_t pad;
  struct {
    uint64_t addr;
    uint32_t ctrl;
    uint32_t pad;
  } dbg_regs[16];
};
bool g_ptrace = false;
std::vector<BpInfo> g_ptrace_bps;
user_hwdebug_state g_break{};
user_hwdebug_state g_watch{};
int g_hw_break_slots = 6;  // 从 GETREGSET dbg_info 更新
int g_hw_watch_slots = 4;
bool g_hw_slots_queried = false;
// 软断点重插地址（定义提前，供 bp_shutdown 清理）
uintptr_t g_rearm_after_step = 0;

void set_st(const char* s) {
  std::snprintf(g_status, sizeof(g_status), "%s", s ? s : "");
}

size_t hwdebug_iov_len(int slots) {
  if (slots < 1) slots = 1;
  if (slots > 16) slots = 16;
  // dbg_info(4)+pad(4) + slots * (addr8+ctrl4+pad4)
  return 8u + (size_t)slots * 16u;
}

int tpid() { return attached_pid(); }

long perf_event_open(struct perf_event_attr* attr, pid_t pid, int cpu,
                     int group_fd, unsigned long flags) {
  return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

int bp_len_from_size(int size) {
  switch (size) {
    case 1:
      return HW_BREAKPOINT_LEN_1;
    case 2:
      return HW_BREAKPOINT_LEN_2;
    case 8:
      return HW_BREAKPOINT_LEN_8;
    default:
      return HW_BREAKPOINT_LEN_4;
  }
}

int bp_type_to_hw(BpType t) {
  switch (t) {
    case BpType::Exec:
      return HW_BREAKPOINT_X;
    case BpType::WatchW:
      return HW_BREAKPOINT_W;
    case BpType::WatchR:
      return HW_BREAKPOINT_R;
    case BpType::WatchRW:
      return HW_BREAKPOINT_RW;
    default:
      return HW_BREAKPOINT_W;
  }
}

void push_hit(const BpHit& h) {
  g_hit_log.push_front(h);
  while (g_hit_log.size() > kMaxLog) g_hit_log.pop_back();
}

// ── ptrace helpers ────────────────────────────────────────
// AArch64 MDSCR/DBGBCR: enable | PMC=EL0 | BAS
uint32_t make_break_ctrl() { return 0x1 | (2u << 1) | (0xFu << 5); }
// watch: enable | PMC=EL0 | LSC | BAS；addr 按 8 对齐时 BAS 选子字节
uint32_t make_watch_ctrl(int size, bool write_only, uintptr_t addr) {
  int off = (int)(addr & 7ull);
  if (size != 1 && size != 2 && size != 4 && size != 8) size = 4;
  if (off + size > 8) {
    // 跨 8 字节边界：退化为整 8 字节监视
    off = 0;
    size = 8;
  }
  uint32_t bas = 0;
  for (int i = 0; i < size; ++i) bas |= (1u << (off + i));
  if (!bas) bas = 0x0F;
  uint32_t lsc = write_only ? 0x2u : 0x3u;  // store / load+store
  // WatchR only
  if (!write_only && size == 0) lsc = 0x1u;
  return 0x1u | (2u << 1) | (lsc << 3) | (bas << 5);
}
uint32_t make_watch_ctrl_type(BpType t, int size, uintptr_t addr) {
  if (t == BpType::WatchR) {
    int off = (int)(addr & 7ull);
    if (size != 1 && size != 2 && size != 4 && size != 8) size = 4;
    if (off + size > 8) { off = 0; size = 8; }
    uint32_t bas = 0;
    for (int i = 0; i < size; ++i) bas |= (1u << (off + i));
    return 0x1u | (2u << 1) | (0x1u << 3) | (bas << 5);  // load only
  }
  bool wo = (t == BpType::WatchW);
  return make_watch_ctrl(size, wo, addr);
}

bool ptrace_set_regset(int type, void* buf, size_t len) {
  struct iovec iov{buf, len};
  return ptrace(PTRACE_SETREGSET, tpid(), (void*)(uintptr_t)type, &iov) == 0;
}

bool ptrace_get_regset(int type, void* buf, size_t len) {
  struct iovec iov{buf, len};
  return ptrace(PTRACE_GETREGSET, tpid(), (void*)(uintptr_t)type, &iov) == 0;
}

void query_hw_slots() {
  if (g_hw_slots_queried || tpid() <= 0) return;
  user_hwdebug_state br{}, wa{};
  if (ptrace_get_regset(NT_ARM_HW_BREAK, &br, sizeof(br))) {
    int n = (int)(br.dbg_info & 0xff);
    if (n > 0 && n <= 16) g_hw_break_slots = n;
  }
  if (ptrace_get_regset(NT_ARM_HW_WATCH, &wa, sizeof(wa))) {
    int n = (int)(wa.dbg_info & 0xff);
    if (n > 0 && n <= 16) g_hw_watch_slots = n;
  }
  g_hw_slots_queried = true;
}

bool ptrace_read_regs(uint64_t* pc, uint64_t* lr, uint64_t* sp, uint64_t* fp) {
  struct user_pt_regs {
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
  } r{};
  struct iovec iov{&r, sizeof(r)};
  if (ptrace(PTRACE_GETREGSET, tpid(), (void*)(uintptr_t)NT_PRSTATUS, &iov) != 0)
    return false;
  if (pc) *pc = r.pc;
  if (sp) *sp = r.sp;
  if (lr) *lr = r.regs[30];
  if (fp) *fp = r.regs[29];
  return true;
}

bool ptrace_push() {
  query_hw_slots();
  std::memset(&g_break, 0, sizeof(g_break));
  std::memset(&g_watch, 0, sizeof(g_watch));
  int bi = 0, wi = 0;
  for (auto& b : g_ptrace_bps) {
    if (!b.active || !b.enabled) continue;
    if (b.type == BpType::Exec) {
      if (bi >= g_hw_break_slots) continue;
      g_break.dbg_regs[bi].addr = b.addr & ~3ull;
      g_break.dbg_regs[bi].ctrl = make_break_ctrl();
      b.slot = bi++;
    } else {
      if (wi >= g_hw_watch_slots) continue;
      // 地址 8 字节对齐，BAS 选子字节
      g_watch.dbg_regs[wi].addr = b.addr & ~7ull;
      g_watch.dbg_regs[wi].ctrl =
          make_watch_ctrl_type(b.type, b.size, b.addr);
      b.slot = wi++;
    }
  }
  // 关键：iov 长度必须匹配实际槽位数，否则内核返回 ENOSPC
  size_t blen = hwdebug_iov_len(g_hw_break_slots);
  size_t wlen = hwdebug_iov_len(g_hw_watch_slots);
  bool ok1 = ptrace_set_regset(NT_ARM_HW_BREAK, &g_break, blen);
  bool ok2 = ptrace_set_regset(NT_ARM_HW_WATCH, &g_watch, wlen);
  if (!ok1 && bi > 0) {
    ok1 = ptrace_set_regset(NT_ARM_HW_BREAK, &g_break,
                            hwdebug_iov_len(bi));
  }
  if (!ok2 && wi > 0) {
    ok2 = ptrace_set_regset(NT_ARM_HW_WATCH, &g_watch,
                            hwdebug_iov_len(wi));
  }
  // 无断点时也要清寄存器（全 0）
  if (bi == 0) ok1 = ptrace_set_regset(NT_ARM_HW_BREAK, &g_break, blen) || true;
  if (wi == 0) ok2 = ptrace_set_regset(NT_ARM_HW_WATCH, &g_watch, wlen) || true;
  return (bi == 0 || ok1) && (wi == 0 || ok2);
}

bool try_ptrace_attach() {
  int p = tpid();
  if (p <= 0) return false;
  if (g_ptrace) {
    // 确保处于可写寄存器状态：若在跑则 interrupt
    if (!g_stopped && !g_dbg_paused) {
      if (ptrace(PTRACE_INTERRUPT, p, nullptr, nullptr) == 0) {
        int st = 0;
        waitpid(p, &st, __WALL);
        g_stopped = true;
        g_dbg_paused = true;
      }
    }
    query_hw_slots();
    return true;
  }
  long opts = PTRACE_O_TRACECLONE | PTRACE_O_EXITKILL;
  long r = ptrace(PTRACE_SEIZE, p, nullptr, (void*)(uintptr_t)opts);
  if (r != 0) {
    r = ptrace(PTRACE_ATTACH, p, nullptr, nullptr);
    if (r != 0) return false;
    int st = 0;
    waitpid(p, &st, __WALL);
    g_stopped = true;
    g_dbg_paused = true;
    ptrace(PTRACE_SETOPTIONS, p, nullptr, (void*)(uintptr_t)opts);
  } else {
    ptrace(PTRACE_INTERRUPT, p, nullptr, nullptr);
    int st = 0;
    waitpid(p, &st, __WALL);
    g_stopped = true;
    g_dbg_paused = true;
  }
  g_ptrace = true;
  g_focus_tid = p;
  query_hw_slots();
  return true;
}

void close_perf_bp(PerfBp& b) {
  if (b.mmap_base && b.mmap_base != MAP_FAILED) {
    munmap(b.mmap_base, b.mmap_size);
    b.mmap_base = nullptr;
  }
  if (b.fd >= 0) {
    close(b.fd);
    b.fd = -1;
  }
}

bool open_perf_for(PerfBp& b) {
  close_perf_bp(b);
  int p = tpid();
  if (p <= 0) return false;

  auto try_open = [&](int precise, int inherit, int sample) -> long {
    struct perf_event_attr attr {};
    std::memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_BREAKPOINT;
    attr.size = sizeof(attr);
    attr.bp_type = bp_type_to_hw(b.info.type);
    attr.bp_len = (b.info.type == BpType::Exec)
                      ? HW_BREAKPOINT_LEN_4
                      : bp_len_from_size(b.info.size);
    if (b.info.type != BpType::Exec) {
      int al = b.info.size > 0 ? b.info.size : 4;
      attr.bp_addr = b.info.addr & ~((uint64_t)al - 1);
    } else {
      attr.bp_addr = b.info.addr & ~3ull;
    }
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.disabled = b.info.enabled ? 0 : 1;
    attr.precise_ip = precise;
    attr.inherit = inherit;
    if (sample) {
      attr.sample_period = 1;
      attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID;
      attr.wakeup_events = 1;
    }
    errno = 0;
    return perf_event_open(&attr, p, -1, -1, PERF_FLAG_FD_CLOEXEC);
  };

  // 多组参数尝试（Android 上 inherit/sample 差异大）
  long fd = -1;
  int last_err = 0;
  const int tries[][3] = {
      {0, 0, 1}, {0, 1, 1}, {0, 0, 0}, {0, 1, 0}, {2, 0, 1},
  };
  for (auto& t : tries) {
    fd = try_open(t[0], t[1], t[2]);
    last_err = errno;
    if (fd >= 0) break;
  }
  if (fd < 0) {
    char buf[120];
    std::snprintf(buf, sizeof(buf), "perf 失败 errno=%d %s", last_err,
                  std::strerror(last_err));
    set_st(buf);
    return false;
  }

  b.fd = (int)fd;
  g_page = sysconf(_SC_PAGESIZE);
  if (g_page < 4096) g_page = 4096;
  b.mmap_size = (size_t)g_page * 2;
  b.mmap_base =
      mmap(nullptr, b.mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, b.fd, 0);
  if (b.mmap_base == MAP_FAILED) b.mmap_base = nullptr;

  fcntl(b.fd, F_SETFL, O_NONBLOCK);
  ioctl(b.fd, PERF_EVENT_IOC_RESET, 0);
  if (b.info.enabled) {
    ioctl(b.fd, PERF_EVENT_IOC_ENABLE, 0);
    ioctl(b.fd, PERF_EVENT_IOC_REFRESH, 1);
  }

  uint64_t c = 0;
  if (read(b.fd, &c, sizeof(c)) == (ssize_t)sizeof(c)) b.last_count = c;
  else b.last_count = 0;
  return true;
}

// 解析 mmap 中最新 sample 的 IP
bool consume_sample_ip(PerfBp& b, uint64_t& out_ip) {
  out_ip = 0;
  if (!b.mmap_base) return false;
  auto* meta = (struct perf_event_mmap_page*)b.mmap_base;
  uint64_t head = meta->data_head;
  __sync_synchronize();
  uint64_t tail = meta->data_tail;
  if (head == tail) return false;

  char* data = (char*)b.mmap_base + g_page;
  size_t data_size = (size_t)g_page;
  // 只取最新一条
  uint64_t pos = tail;
  bool got = false;
  while (pos < head) {
    size_t off = pos % data_size;
    auto* eh = (struct perf_event_header*)(data + off);
    // 边界简单处理：header 必须连续
    if (off + sizeof(*eh) > data_size) break;
    uint16_t sz = eh->size;
    if (sz < sizeof(*eh) || pos + sz > head) break;
    if (eh->type == PERF_RECORD_SAMPLE) {
      // sample: header + IP (+ optional)
      if (off + sizeof(*eh) + 8 <= data_size) {
        uint64_t ip = 0;
        std::memcpy(&ip, data + off + sizeof(*eh), 8);
        out_ip = ip;
        got = true;
      }
    }
    pos += sz;
  }
  meta->data_tail = head;
  __sync_synchronize();
  return got;
}

bool poll_perf_hits(BpHit& hit) {
  hit = {};
  std::lock_guard<std::mutex> lk(g_mu);
  for (auto& b : g_perf) {
    if (!b.info.active || !b.info.enabled || b.fd < 0) continue;

    // 1) 尝试 sample
    uint64_t ip = 0;
    bool from_sample = consume_sample_ip(b, ip);

    // 2) count 变化
    uint64_t c = 0;
    ssize_t nr = read(b.fd, &c, sizeof(c));
    bool count_up = false;
    if (nr == (ssize_t)sizeof(c) && c > b.last_count) {
      count_up = true;
      b.last_count = c;
    } else if (nr < 0 && errno != EAGAIN) {
      // ignore
    }

    if (!from_sample && !count_up) continue;

    // 条件断点：尽量读寄存器求值（需已 ptrace）
    if (b.info.cond[0]) {
      Regs rr{};
      bool have_regs = false;
      if (g_ptrace) {
        user_pt_regs ur{};
        iovec iv{&ur, sizeof(ur)};
        int tid = g_focus_tid > 0 ? g_focus_tid : tpid();
        if (ptrace(PTRACE_GETREGSET, tid, (void*)(uintptr_t)NT_PRSTATUS,
                   &iv) == 0) {
          for (int i = 0; i < 31; ++i) rr.x[i] = ur.regs[i];
          rr.sp = ur.sp;
          rr.pc = ur.pc;
          rr.pstate = ur.pstate;
          rr.valid = true;
          have_regs = true;
        }
      }
      if (have_regs && !eval_condition_impl(b.info.cond, rr)) {
        ioctl(b.fd, PERF_EVENT_IOC_REFRESH, 1);
        continue;
      }
      if (!have_regs) {
        // 无寄存器：无法评估条件，仍报命中但标注
        // （避免静默丢断点；UI 可见 cond?）
      }
    }

    b.info.hit_count++;
    hit.valid = true;
    hit.bp_id = b.info.id;
    hit.type = b.info.type;
    hit.watch_addr = b.info.addr;
    hit.pc = from_sample ? (uintptr_t)ip : 0;
    hit.hit_index = b.info.hit_count;
    const char* ctag = "";
    if (b.info.cond[0]) {
      ctag = g_ptrace ? " (cond)" : " (cond未评估:需ptrace)";
    }
    std::snprintf(hit.msg, sizeof(hit.msg),
                  "[perf] %s 命中#%llu @0x%llX IP=0x%llX%s",
                  bp_type_name(b.info.type),
                  (unsigned long long)b.info.hit_count,
                  (unsigned long long)b.info.addr,
                  (unsigned long long)hit.pc, ctag);
    push_hit(hit);
    set_st(hit.msg);

    // 刷新 event
    ioctl(b.fd, PERF_EVENT_IOC_REFRESH, 1);
    return true;
  }
  return false;
}

bool poll_ptrace_hits(BpHit& hit) {
  hit = {};
  if (!g_ptrace || g_stopped) return false;
  int st = 0;
  pid_t w = waitpid(-1, &st, WNOHANG | __WALL);
  if (w <= 0) return false;
  if (!WIFSTOPPED(st)) {
    if (WIFEXITED(st) || WIFSIGNALED(st)) {
      set_st("目标已退出");
      g_ptrace = false;
    }
    return false;
  }
  g_stopped = true;
  g_dbg_paused = true;
  g_focus_tid = w;
  int sig = WSTOPSIG(st);
  uint64_t pc = 0, lr = 0, sp = 0, fp = 0;
  ptrace_read_regs(&pc, &lr, &sp, &fp);
  hit.valid = true;
  hit.pc = (uintptr_t)pc;
  hit.lr = (uintptr_t)lr;
  hit.sp = (uintptr_t)sp;
  hit.fp = (uintptr_t)fp;
  hit.tid = w;
  hit.type = BpType::Exec;
  bool matched = false;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& b : g_ptrace_bps) {
      if (!b.active || !b.enabled) continue;
      if (b.type == BpType::Exec && (b.addr & ~3ull) == (pc & ~3ull)) {
        hit.type = BpType::Exec;
        hit.watch_addr = b.addr;
        hit.bp_id = b.id;
        b.hit_count++;
        hit.hit_index = b.hit_count;
        matched = true;
        break;
      }
    }
    // 观察点：PC 不在断点地址，按最近启用的 watch 归类
    if (!matched) {
      for (auto& b : g_ptrace_bps) {
        if (!b.active || !b.enabled) continue;
        if (b.type == BpType::Exec) continue;
        hit.type = b.type;
        hit.watch_addr = b.addr;
        hit.bp_id = b.id;
        b.hit_count++;
        hit.hit_index = b.hit_count;
        matched = true;
        break;
      }
    }
    push_hit(hit);
  }
  std::snprintf(hit.msg, sizeof(hit.msg),
                "[hw] %s 命中#%llu PC=0x%llX @0x%llX tid=%d sig=%d",
                matched ? bp_type_name(hit.type) : "停止",
                (unsigned long long)hit.hit_index, (unsigned long long)pc,
                (unsigned long long)hit.watch_addr, w, sig);
  set_st(hit.msg);
  return true;
}

}  // namespace

const char* bp_type_name(BpType t) {
  switch (t) {
    case BpType::Exec:
      return "执行";
    case BpType::WatchW:
      return "写";
    case BpType::WatchR:
      return "读";
    case BpType::WatchRW:
      return "读写";
    default:
      return "?";
  }
}

bool bp_init() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (tpid() <= 0) {
    set_st("请先附加进程");
    g_backend = BpBackend::None;
    return false;
  }
  // 探测 perf：在自身或目标上开一个 dummy 很快关掉
  // 直接标记为 Perf，真正失败在 bp_set 时回退
  g_backend = BpBackend::Perf;
  set_st("后端: perf_event 硬件断点");
  return true;
}

void bp_shutdown() {
  // 先清硬件/软断点，避免 DETACH 后目标踩到残留 BRK/HW
  soft_bp_clear_all();
  bp_clear_all();
  if (g_ptrace && tpid() > 0) {
    int p = tpid();
    // 吞掉挂起的 stop，DETACH 时不投递 SIGTRAP（否则目标被杀）
    int st = 0;
    for (int i = 0; i < 8; ++i) {
      pid_t w = waitpid(-1, &st, WNOHANG | __WALL);
      if (w <= 0) break;
    }
    // 若仍停着，先 CONT(无信号) 再 DETACH
    if (g_stopped || g_dbg_paused) {
      ptrace(PTRACE_CONT, p, nullptr, nullptr);
      g_stopped = false;
      g_dbg_paused = false;
    }
    ptrace(PTRACE_DETACH, p, nullptr, nullptr);
  }
  g_ptrace = false;
  g_stopped = false;
  g_dbg_paused = false;
  g_focus_tid = 0;
  g_hw_slots_queried = false;
  g_backend = BpBackend::None;
  g_rearm_after_step = 0;
  set_st("断点已关闭");
}

bool bp_ready() { return tpid() > 0 && g_backend != BpBackend::None; }

BpBackend bp_backend() { return g_backend; }

const char* bp_backend_name() {
  switch (g_backend) {
    case BpBackend::Perf:
      return "perf";
    case BpBackend::Ptrace:
      return "ptrace";
    default:
      return "none";
  }
}

bool bp_attach_ptrace() { return bp_init(); }
void bp_detach_ptrace() { bp_shutdown(); }
bool bp_ptrace_active() {
  return g_backend == BpBackend::Perf || g_ptrace;
}

int bp_set(uintptr_t addr, BpType type, int size) {
  if (tpid() <= 0) {
    set_st("未附加进程");
    return -1;
  }
  if (size != 1 && size != 2 && size != 4 && size != 8) size = 4;
  if (g_backend == BpBackend::None) bp_init();

  std::lock_guard<std::mutex> lk(g_mu);

  // 已存在则更新
  if (g_backend == BpBackend::Perf || g_backend == BpBackend::None) {
    for (auto& b : g_perf) {
      if (b.info.active && b.info.addr == addr && b.info.type == type) {
        b.info.size = size;
        b.info.enabled = true;
        if (open_perf_for(b)) {
          set_st("perf 断点已更新");
          g_backend = BpBackend::Perf;
          return b.info.id;
        }
        break;
      }
    }

    PerfBp b;
    b.info.id = g_next_id++;
    b.info.addr = addr;
    b.info.type = type;
    b.info.size = size;
    b.info.active = true;
    b.info.enabled = true;
    b.info.slot = (int)g_perf.size();
    std::snprintf(b.info.note, sizeof(b.info.note), "perf");

    if (open_perf_for(b)) {
      g_perf.push_back(b);
      g_backend = BpBackend::Perf;
      char buf[96];
      std::snprintf(buf, sizeof(buf), "perf 断点#%d %s @0x%llX", b.info.id,
                    bp_type_name(type), (unsigned long long)addr);
      set_st(buf);
      return b.info.id;
    }

    // perf 失败 → 回退 ptrace
    close_perf_bp(b);
    set_st("perf 失败，尝试 ptrace…");
  }

  // ptrace 硬件断点路径（Android 上 perf 常 ENOSPC，此为可靠路径）
  if (!try_ptrace_attach()) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "断点失败: perf/ptrace 均不可用 (%s)",
                  std::strerror(errno));
    set_st(buf);
    return -1;
  }
  g_backend = BpBackend::Ptrace;
  // 槽位检查
  query_hw_slots();
  int used_b = 0, used_w = 0;
  for (auto& b : g_ptrace_bps) {
    if (!b.active || !b.enabled) continue;
    if (b.type == BpType::Exec) used_b++;
    else used_w++;
  }
  for (auto& b : g_ptrace_bps) {
    if (b.active && b.addr == addr && b.type == type) {
      b.size = size;
      b.enabled = true;
      if (!ptrace_push()) {
        set_st("ptrace 更新断点失败");
        return -1;
      }
      // 下断后继续运行，等待命中
      if (g_stopped || g_dbg_paused) {
        ptrace(PTRACE_CONT, tpid(), nullptr, nullptr);
        g_stopped = false;
        g_dbg_paused = false;
      }
      set_st("ptrace 断点已更新");
      return b.id;
    }
  }
  if (type == BpType::Exec && used_b >= g_hw_break_slots) {
    char buf[80];
    std::snprintf(buf, sizeof(buf), "执行断点槽已满(%d)", g_hw_break_slots);
    set_st(buf);
    return -1;
  }
  if (type != BpType::Exec && used_w >= g_hw_watch_slots) {
    char buf[80];
    std::snprintf(buf, sizeof(buf), "观察点槽已满(%d)", g_hw_watch_slots);
    set_st(buf);
    return -1;
  }
  BpInfo info;
  info.id = g_next_id++;
  info.addr = addr;
  info.type = type;
  info.size = size;
  info.active = true;
  info.enabled = true;
  std::snprintf(info.note, sizeof(info.note), "hw%d/%d", g_hw_break_slots,
                g_hw_watch_slots);
  g_ptrace_bps.push_back(info);
  if (!ptrace_push()) {
    g_ptrace_bps.pop_back();
    char buf[96];
    std::snprintf(buf, sizeof(buf), "ptrace 硬件寄存器写入失败 (brk=%d watch=%d)",
                  g_hw_break_slots, g_hw_watch_slots);
    set_st(buf);
    return -1;
  }
  // 下断后让目标继续跑，否则永远等不到命中
  if (g_stopped || g_dbg_paused) {
    ptrace(PTRACE_CONT, focus_tid() > 0 ? focus_tid() : tpid(), nullptr,
           nullptr);
    g_stopped = false;
    g_dbg_paused = false;
  }
  char buf[120];
  std::snprintf(buf, sizeof(buf), "硬件断点#%d %s @0x%llX (ptrace %dB/%dW)",
                info.id, bp_type_name(type), (unsigned long long)addr,
                g_hw_break_slots, g_hw_watch_slots);
  set_st(buf);
  return info.id;
}

bool bp_clear(int id) {
  std::lock_guard<std::mutex> lk(g_mu);
  bool any = false;
  for (auto it = g_perf.begin(); it != g_perf.end();) {
    if (it->info.id == id) {
      close_perf_bp(*it);
      it = g_perf.erase(it);
      any = true;
    } else
      ++it;
  }
  for (auto& b : g_ptrace_bps) {
    if (b.id == id && b.active) {
      b.active = false;
      any = true;
    }
  }
  if (g_ptrace) ptrace_push();
  if (any) set_st("已删除断点");
  return any;
}

void bp_clear_all() {
  std::lock_guard<std::mutex> lk(g_mu);
  for (auto& b : g_perf) close_perf_bp(b);
  g_perf.clear();
  for (auto& b : g_ptrace_bps) b.active = false;
  g_ptrace_bps.clear();
  std::memset(&g_break, 0, sizeof(g_break));
  std::memset(&g_watch, 0, sizeof(g_watch));
  if (g_ptrace && tpid() > 0) {
    query_hw_slots();
    ptrace_set_regset(NT_ARM_HW_BREAK, &g_break,
                      hwdebug_iov_len(g_hw_break_slots));
    ptrace_set_regset(NT_ARM_HW_WATCH, &g_watch,
                      hwdebug_iov_len(g_hw_watch_slots));
  }
  set_st("已清除全部断点");
}

bool bp_enable(int id, bool on) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (auto& b : g_perf) {
    if (b.info.id == id && b.fd >= 0) {
      b.info.enabled = on;
      ioctl(b.fd, on ? PERF_EVENT_IOC_ENABLE : PERF_EVENT_IOC_DISABLE, 0);
      return true;
    }
  }
  for (auto& b : g_ptrace_bps) {
    if (b.id == id) {
      b.enabled = on;
      ptrace_push();
      return true;
    }
  }
  return false;
}

std::vector<BpInfo> bp_list() {
  std::lock_guard<std::mutex> lk(g_mu);
  std::vector<BpInfo> out;
  for (auto& b : g_perf)
    if (b.info.active) out.push_back(b.info);
  for (auto& b : g_ptrace_bps)
    if (b.active) out.push_back(b);
  return out;
}

int bp_count() {
  std::lock_guard<std::mutex> lk(g_mu);
  int n = 0;
  for (auto& b : g_perf)
    if (b.info.active) n++;
  for (auto& b : g_ptrace_bps)
    if (b.active) n++;
  return n;
}

int bp_max_slots() {
  // ARM 常见 4~6 数据 + 6 指令；perf 由内核调度
  return 8;
}

bool bp_arm_and_continue() {
  // 与 dbg_resume 统一：含软断点单步重插
  if (g_ptrace) return dbg_resume();
  {
    std::lock_guard<std::mutex> lk(g_mu);
    rearm_all_soft_bps_locked();
  }
  if (g_backend == BpBackend::Perf) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& b : g_perf) {
      if (b.fd >= 0 && b.info.enabled) {
        ioctl(b.fd, PERF_EVENT_IOC_ENABLE, 0);
        ioctl(b.fd, PERF_EVENT_IOC_REFRESH, 1);
      }
    }
    set_st("perf 监听中…");
    return true;
  }
  set_st("无活动后端");
  return false;
}

bool bp_poll(BpHit& hit) {
  // perf 与 ptrace 可能并存（perf 失败回退 ptrace 后 g_backend=Ptrace）
  if (g_backend == BpBackend::Perf || !g_perf.empty()) {
    if (poll_perf_hits(hit)) return true;
  }
  if (g_ptrace || g_backend == BpBackend::Ptrace) {
    if (poll_ptrace_hits(hit)) return true;
  }
  hit = {};
  return false;
}

bool bp_continue() { return bp_arm_and_continue(); }

bool bp_is_stopped() { return g_stopped; }

std::vector<BpHit> bp_hit_log(size_t max_n) {
  std::lock_guard<std::mutex> lk(g_mu);
  std::vector<BpHit> out;
  size_t n = 0;
  for (auto& h : g_hit_log) {
    if (n++ >= max_n) break;
    out.push_back(h);
  }
  return out;
}

void bp_clear_hit_log() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_hit_log.clear();
}

const char* bp_status() { return g_status; }

// ── 动态调试：寄存器 / 单步 / 软断点 / 补丁 ────────────────
// 临时断点（步过/步出）
int g_temp_soft_id = -1;
uintptr_t g_temp_soft_addr = 0;
// g_rearm_after_step 已在文件前部定义
// 最近一次命中（供 UI 弹寄存器）
BpHit g_last_dbg_hit{};
bool g_have_dbg_hit = false;

void rearm_all_soft_bps_locked() {
  uint32_t brk = 0xD4200000u;
  for (auto& s : g_soft) {
    if (!s.active) continue;
    // 若当前正停在该地址待单步重插，不要立刻插回 BRK（否则死循环）
    if (g_rearm_after_step && s.addr == g_rearm_after_step) continue;
    uint32_t cur = 0;
    if (!read_mem(s.addr, &cur, 4)) continue;
    if (cur != brk) write_mem(s.addr, &brk, 4);
  }
}

/** 执行原指令一拍后插回 BRK；keep_paused=true 时停在单步后 */
bool soft_bp_rearm_step_locked(bool keep_paused) {
  if (!g_rearm_after_step) return false;
  int tid = focus_tid();
  uintptr_t rearm = g_rearm_after_step;
  g_rearm_after_step = 0;
  uint32_t brk = 0xD4200000u;
  if (ptrace(PTRACE_SINGLESTEP, tid, nullptr, nullptr) == 0) {
    int st = 0;
    waitpid(tid, &st, __WALL);
  }
  write_mem(rearm, &brk, 4);
  rearm_all_soft_bps_locked();
  if (keep_paused) {
    g_stopped = true;
    g_dbg_paused = true;
  }
  return true;
}

#ifndef NT_ARM_VFP
#define NT_ARM_VFP 0x400
#endif
#ifndef NT_PRFPREG
#define NT_PRFPREG 2
#endif
#ifndef PTRACE_O_TRACECLONE
#define PTRACE_O_TRACECLONE (1 << 3)
#endif
#ifndef PTRACE_O_EXITKILL
#define PTRACE_O_EXITKILL (1 << 20)
#endif

int focus_tid() {
  if (g_focus_tid > 0) return g_focus_tid;
  return tpid();
}

bool ensure_ptrace() {
  if (tpid() <= 0) {
    set_st("未附加进程");
    return false;
  }
  if (g_ptrace) return true;
  long opts = PTRACE_O_TRACECLONE | PTRACE_O_EXITKILL;
  if (ptrace(PTRACE_SEIZE, tpid(), nullptr, (void*)(uintptr_t)opts) != 0) {
    if (ptrace(PTRACE_ATTACH, tpid(), nullptr, nullptr) != 0) {
      set_st("ptrace attach 失败");
      return false;
    }
    int st = 0;
    waitpid(tpid(), &st, 0);
    g_stopped = true;
    g_dbg_paused = true;
    ptrace(PTRACE_SETOPTIONS, tpid(), nullptr, (void*)(uintptr_t)opts);
  }
  g_ptrace = true;
  g_focus_tid = tpid();
  return true;
}

bool regs_get(user_pt_regs& r) {
  int tid = focus_tid();
  iovec iv{&r, sizeof(r)};
  if (ptrace(PTRACE_GETREGSET, tid, (void*)(uintptr_t)NT_PRSTATUS, &iv) != 0)
    return false;
  return true;
}

bool regs_set(const user_pt_regs& r) {
  int tid = focus_tid();
  iovec iv{const_cast<user_pt_regs*>(&r), sizeof(r)};
  return ptrace(PTRACE_SETREGSET, tid, (void*)(uintptr_t)NT_PRSTATUS, &iv) == 0;
}

bool eval_condition(const char* cond, const Regs& r) {
  return eval_condition_impl(cond, r);
}

bool dbg_pause() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!ensure_ptrace()) return false;
  if (g_dbg_paused) {
    set_st("已暂停");
    return true;
  }
  if (ptrace(PTRACE_INTERRUPT, tpid(), nullptr, nullptr) != 0) {
    // ATTACH 已停
    if (!g_stopped) {
      set_st("暂停失败");
      return false;
    }
  } else {
    int st = 0;
    waitpid(tpid(), &st, 0);
  }
  g_stopped = true;
  g_dbg_paused = true;
  set_st("目标已暂停");
  return true;
}

bool dbg_resume() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (tpid() <= 0) return false;
  if (!g_ptrace) {
    set_st("未 ptrace");
    return false;
  }
  int tid = focus_tid();

  // 命中软断点后：先单步执行原指令，再插回 BRK，避免死循环
  if (g_rearm_after_step) {
    soft_bp_rearm_step_locked(false);
    if (ptrace(PTRACE_CONT, tid, nullptr, nullptr) != 0 &&
        ptrace(PTRACE_CONT, tpid(), nullptr, nullptr) != 0) {
      set_st("继续失败");
      return false;
    }
    g_stopped = false;
    g_dbg_paused = false;
    set_st("目标运行中(软断点已重插)");
    return true;
  }

  rearm_all_soft_bps_locked();
  if (ptrace(PTRACE_CONT, tid, nullptr, nullptr) != 0) {
    if (ptrace(PTRACE_CONT, tpid(), nullptr, nullptr) != 0) {
      set_st("继续失败");
      return false;
    }
  }
  g_stopped = false;
  g_dbg_paused = false;
  set_st("目标运行中");
  return true;
}

bool dbg_step() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!ensure_ptrace()) return false;
  int tid = focus_tid();
  if (!g_dbg_paused) {
    if (ptrace(PTRACE_INTERRUPT, tid, nullptr, nullptr) == 0) {
      int st = 0;
      waitpid(tid, &st, __WALL);
      g_dbg_paused = true;
      g_stopped = true;
    }
  }
  // 软断点命中后步入：单步原指令并重插 BRK，保持暂停
  if (g_rearm_after_step) {
    if (!soft_bp_rearm_step_locked(true)) {
      set_st("步入(重插)失败");
      return false;
    }
    set_st("步入完成(软断点已重插)");
    return true;
  }
  if (ptrace(PTRACE_SINGLESTEP, tid, nullptr, nullptr) != 0) {
    set_st("单步失败");
    return false;
  }
  int st = 0;
  waitpid(tid, &st, __WALL);
  g_dbg_paused = true;
  g_stopped = true;
  set_st("步入完成");
  return true;
}

bool dbg_is_paused() { return g_dbg_paused; }

bool dbg_set_tid(int tid) {
  if (tid < 0) return false;
  g_focus_tid = tid;
  char b[48];
  std::snprintf(b, sizeof(b), "调试线程 tid=%d", tid > 0 ? tid : tpid());
  set_st(b);
  return true;
}

int dbg_get_tid() { return focus_tid(); }

bool dbg_attach_thread(int tid) {
  if (tid <= 0) return false;
  std::lock_guard<std::mutex> lk(g_mu);
  if (!ensure_ptrace()) return false;
  long opts = PTRACE_O_TRACECLONE;
  if (ptrace(PTRACE_SEIZE, tid, nullptr, (void*)(uintptr_t)opts) != 0) {
    if (ptrace(PTRACE_ATTACH, tid, nullptr, nullptr) != 0) {
      set_st("attach 线程失败");
      return false;
    }
    int st = 0;
    waitpid(tid, &st, 0);
  }
  g_focus_tid = tid;
  g_dbg_paused = true;
  g_stopped = true;
  set_st("已附加线程");
  return true;
}

bool dbg_regs_read(Regs& out) {
  out = {};
  std::lock_guard<std::mutex> lk(g_mu);
  if (!ensure_ptrace()) return false;
  bool need_resume = false;
  if (!g_dbg_paused) {
    if (ptrace(PTRACE_INTERRUPT, tpid(), nullptr, nullptr) == 0) {
      int st = 0;
      waitpid(tpid(), &st, 0);
      g_dbg_paused = true;
      g_stopped = true;
      need_resume = true;
    }
  }
  user_pt_regs r{};
  bool ok = regs_get(r);
  if (ok) {
    for (int i = 0; i < 31; ++i) out.x[i] = r.regs[i];
    out.sp = r.sp;
    out.pc = r.pc;
    out.pstate = r.pstate;
    out.valid = true;
  }
  if (need_resume) {
    ptrace(PTRACE_CONT, tpid(), nullptr, nullptr);
    g_dbg_paused = false;
    g_stopped = false;
  }
  if (!ok) set_st("读寄存器失败");
  return ok;
}

bool dbg_regs_write(const Regs& in) {
  if (!in.valid) return false;
  std::lock_guard<std::mutex> lk(g_mu);
  if (!ensure_ptrace()) return false;
  if (!g_dbg_paused) {
    if (ptrace(PTRACE_INTERRUPT, tpid(), nullptr, nullptr) == 0) {
      int st = 0;
      waitpid(tpid(), &st, 0);
      g_dbg_paused = true;
      g_stopped = true;
    } else if (!g_stopped) {
      set_st("写寄存器需先暂停");
      return false;
    }
  }
  user_pt_regs r{};
  if (!regs_get(r)) {
    set_st("写寄存器：读失败");
    return false;
  }
  for (int i = 0; i < 31; ++i) r.regs[i] = in.x[i];
  r.sp = in.sp;
  r.pc = in.pc;
  r.pstate = in.pstate;
  if (!regs_set(r)) {
    set_st("写寄存器失败");
    return false;
  }
  set_st("寄存器已写入");
  return true;
}

bool dbg_reg_set(const char* name, uint64_t val) {
  if (!name) return false;
  Regs r;
  if (!dbg_regs_read(r)) return false;
  if (std::strcmp(name, "sp") == 0)
    r.sp = val;
  else if (std::strcmp(name, "pc") == 0)
    r.pc = val;
  else if (name[0] == 'x' || name[0] == 'X' || name[0] == 'w' ||
           name[0] == 'W') {
    int n = std::atoi(name + 1);
    if (n < 0 || n > 30) return false;
    r.x[n] = val;
  } else
    return false;
  r.valid = true;
  return dbg_regs_write(r);
}

int soft_bp_set_cond(uintptr_t addr, const char* cond, bool oneshot) {
  addr &= ~3ull;
  std::lock_guard<std::mutex> lk(g_mu);
  if (tpid() <= 0) {
    set_st("未附加");
    return -1;
  }
  // 先确保 ptrace，失败则不写 BRK，避免目标进程留下陷阱
  if (!ensure_ptrace()) {
    set_st("软断点：ptrace 失败");
    return -1;
  }
  for (auto& s : g_soft) {
    if (s.active && s.addr == addr) {
      if (cond && cond[0])
        std::snprintf(s.cond, sizeof(s.cond), "%s", cond);
      else if (!cond)
        s.cond[0] = 0;
      s.oneshot = oneshot;
      // 确保 BRK 仍在
      uint32_t cur = 0, brk = 0xD4200000u;
      if (read_mem(addr, &cur, 4) && cur != brk) write_mem(addr, &brk, 4);
      return s.id;
    }
  }
  uint32_t orig = 0;
  if (!read_mem(addr, &orig, 4)) {
    set_st("软断点：读失败");
    return -1;
  }
  // 已是 BRK 则拒绝（避免套娃丢 orig）
  if (orig == 0xD4200000u) {
    set_st("软断点：目标已是 BRK");
    return -1;
  }
  uint32_t brk = 0xD4200000u;  // BRK #0
  if (!write_mem(addr, &brk, 4)) {
    set_st("软断点：写失败");
    return -1;
  }
  SoftBp s;
  s.id = g_soft_next++;
  s.addr = addr;
  s.orig = orig;
  s.active = true;
  s.oneshot = oneshot;
  if (cond && cond[0]) std::snprintf(s.cond, sizeof(s.cond), "%s", cond);
  g_soft.push_back(s);
  set_st("软件断点已下(SIGTRAP)");
  return s.id;
}

int soft_bp_set(uintptr_t addr) { return soft_bp_set_cond(addr, nullptr, false); }

bool soft_bp_clear(int id) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (auto it = g_soft.begin(); it != g_soft.end(); ++it) {
    if (it->id == id && it->active) {
      write_mem(it->addr, &it->orig, 4);
      it->active = false;
      g_soft.erase(it);
      set_st("软断点已删除");
      return true;
    }
  }
  return false;
}

void soft_bp_clear_all() {
  std::lock_guard<std::mutex> lk(g_mu);
  for (auto& s : g_soft) {
    if (s.active) write_mem(s.addr, &s.orig, 4);
  }
  g_soft.clear();
  set_st("软断点已清空");
}

std::vector<SoftBp> soft_bp_list() {
  std::lock_guard<std::mutex> lk(g_mu);
  std::vector<SoftBp> out;
  for (auto& s : g_soft)
    if (s.active) out.push_back(s);
  return out;
}

bool patch_nop(uintptr_t addr, int count) {
  if (count < 1) count = 1;
  if (count > 256) count = 256;
  addr &= ~3ull;
  uint32_t nop = 0xD503201Fu;
  for (int i = 0; i < count; ++i) {
    if (!write_mem(addr + (uintptr_t)i * 4, &nop, 4)) {
      set_st("NOP 补丁失败");
      return false;
    }
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "已 NOP %d 条 @0x%llX", count,
                (unsigned long long)addr);
  set_st(buf);
  return true;
}

bool patch_bytes(uintptr_t addr, const uint8_t* data, size_t len) {
  if (!data || len == 0) return false;
  if (!write_mem(addr, data, len)) {
    set_st("字节补丁失败");
    return false;
  }
  set_st("字节补丁成功");
  return true;
}

bool patch_hex(uintptr_t addr, const char* hex) {
  if (!hex) return false;
  std::vector<uint8_t> bytes;
  const char* p = hex;
  while (*p) {
    while (*p == ' ' || *p == ',' || *p == '-') p++;
    if (!*p) break;
    char tmp[3] = {p[0], (char)(p[1] ? p[1] : 0), 0};
    char* end = nullptr;
    unsigned long v = std::strtoul(tmp, &end, 16);
    if (end == tmp) return false;
    bytes.push_back((uint8_t)v);
    p += (tmp[1] ? 2 : 1);
  }
  return patch_bytes(addr, bytes.data(), bytes.size());
}

std::vector<uintptr_t> dbg_stack_trace(int max_frames) {
  std::vector<uintptr_t> out;
  if (max_frames < 1) max_frames = 1;
  if (max_frames > 64) max_frames = 64;
  Regs r;
  if (!dbg_regs_read(r) || !r.valid) return out;
  out.push_back(r.pc);
  // x29 = fp on AAPCS64
  uintptr_t fp = r.x[29];
  uintptr_t lr = r.x[30];
  if (lr) out.push_back(lr);
  for (int i = 0; i < max_frames - 2 && fp > 0x1000; ++i) {
    // frame: [fp] = prev_fp, [fp+8] = lr
    uintptr_t prev_fp = 0, ret = 0;
    if (!read_mem(fp, &prev_fp, 8)) break;
    if (!read_mem(fp + 8, &ret, 8)) break;
    if (ret) out.push_back(ret);
    if (prev_fp <= fp) break;
    fp = prev_fp;
  }
  return out;
}

std::vector<ThreadInfo> list_threads() {
  std::vector<ThreadInfo> out;
  int pid = tpid();
  if (pid <= 0) return out;
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/%d/task", pid);
  DIR* d = opendir(path);
  if (!d) return out;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    ThreadInfo t;
    t.tid = atoi(e->d_name);
    char cp[96], buf[64]{};
    std::snprintf(cp, sizeof(cp), "/proc/%d/task/%d/comm", pid, t.tid);
    FILE* f = fopen(cp, "r");
    if (f) {
      if (fgets(buf, sizeof(buf), f)) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
        std::snprintf(t.name, sizeof(t.name), "%s", buf);
      }
      fclose(f);
    }
    std::snprintf(cp, sizeof(cp), "/proc/%d/task/%d/status", pid, t.tid);
    f = fopen(cp, "r");
    if (f) {
      char line[128];
      while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "State:", 6) == 0) {
          // State:\tS (sleeping)
          char st = ' ';
          sscanf(line + 6, " %c", &st);
          t.state[0] = st;
          t.state[1] = 0;
          break;
        }
      }
      fclose(f);
    }
    out.push_back(t);
  }
  closedir(d);
  return out;
}

std::vector<ModuleInfo> list_modules(const char* filter) {
  std::vector<ModuleInfo> out;
  auto maps = load_maps(false);
  for (auto& r : maps) {
    if (r.path.empty()) continue;
    if (r.path[0] == '[') continue;
    if (filter && filter[0] &&
        r.path.find(filter) == std::string::npos)
      continue;
    // 合并同 path 连续段：简单每段一条
    ModuleInfo m;
    m.start = r.start;
    m.end = r.end;
    std::snprintf(m.perms, sizeof(m.perms), "%s", r.perms);
    m.path = r.path;
    out.push_back(std::move(m));
  }
  return out;
}

std::string module_of(uintptr_t addr) {
  auto maps = load_maps(false);
  for (auto& r : maps) {
    if (addr >= r.start && addr < r.end) {
      if (!r.path.empty()) return r.path;
      return "[anon]";
    }
  }
  return {};
}

// ── 步过 / 步出 ───────────────────────────────────────────
static bool is_bl_insn(uint32_t w) {
  // BL imm26: 100101...
  if ((w >> 26) == 0x25) return true;
  // BLR Xn
  if ((w & 0xFFFFFC1F) == 0xD63F0000) return true;
  return false;
}

bool dbg_step_over() {
  if (!ensure_ptrace()) return false;
  // 若停在软断点待重插，先单步出断点再决定步过
  if (g_rearm_after_step) {
    if (!dbg_step()) return false;
  }
  if (!dbg_is_paused() && !dbg_pause()) return false;
  Regs r;
  if (!dbg_regs_read(r)) return false;
  uint32_t w = 0;
  if (!read_mem(r.pc, &w, 4)) return dbg_step();
  if (!is_bl_insn(w)) return dbg_step();
  // 在下一条设 oneshot 软断点
  uintptr_t next = r.pc + 4;
  int id = soft_bp_set_cond(next, nullptr, true);
  if (id < 0) return dbg_step();
  g_temp_soft_id = id;
  g_temp_soft_addr = next;
  set_st("步过：已下返回点");
  return dbg_resume();
}

bool dbg_step_out() {
  if (!ensure_ptrace()) return false;
  if (g_rearm_after_step) {
    if (!dbg_step()) return false;
  }
  if (!dbg_is_paused() && !dbg_pause()) return false;
  Regs r;
  if (!dbg_regs_read(r)) return false;
  uintptr_t lr = r.x[30];
  if (!lr || (lr & 3)) {
    set_st("LR 无效，无法步出");
    return false;
  }
  int id = soft_bp_set_cond(lr & ~3ull, nullptr, true);
  if (id < 0) return false;
  g_temp_soft_id = id;
  g_temp_soft_addr = lr & ~3ull;
  set_st("步出：断在 LR");
  return dbg_resume();
}

// ── 轮询停止（软断点 / 临时断点 SIGTRAP）────────────────
bool dbg_poll_stop(BpHit& hit) {
  hit = {};
  if (!g_ptrace || tpid() <= 0) return false;
  int st = 0;
  int tid = waitpid(-1, &st, WNOHANG | __WALL);
  if (tid <= 0) return false;
  if (!WIFSTOPPED(st)) return false;
  int sig = WSTOPSIG(st);
  g_focus_tid = tid;
  g_dbg_paused = true;
  g_stopped = true;

  user_pt_regs ur{};
  regs_get(ur);
  Regs r{};
  for (int i = 0; i < 31; ++i) r.x[i] = ur.regs[i];
  r.sp = ur.sp;
  r.pc = ur.pc;
  r.pstate = ur.pstate;
  r.valid = true;

  // AArch64 BRK：PC 通常指向 BRK 指令本身
  uintptr_t pc = ur.pc & ~3ull;
  SoftBp hit_sb{};
  bool have_sb = false;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& s : g_soft) {
      if (!s.active) continue;
      if (s.addr == pc || s.addr + 4 == pc) {
        hit_sb = s;
        have_sb = true;
        break;
      }
    }
  }

  if (have_sb) {
    // 条件不满足：恢复原指令、单步、再下断、继续
    if (hit_sb.cond[0] && !eval_condition_impl(hit_sb.cond, r)) {
      write_mem(hit_sb.addr, &hit_sb.orig, 4);
      // PC 可能在 BRK 后 0/4，强制回到断点地址执行原指令
      ur.pc = hit_sb.addr;
      regs_set(ur);
      ptrace(PTRACE_SINGLESTEP, tid, nullptr, nullptr);
      int st2 = 0;
      waitpid(tid, &st2, __WALL);
      uint32_t brk = 0xD4200000u;
      write_mem(hit_sb.addr, &brk, 4);
      ptrace(PTRACE_CONT, tid, nullptr, nullptr);
      g_dbg_paused = false;
      g_stopped = false;
      return false;
    }
    // 命中：恢复原指令，PC 指回断点处
    write_mem(hit_sb.addr, &hit_sb.orig, 4);
    ur.pc = hit_sb.addr;
    regs_set(ur);
    hit.valid = true;
    hit.is_soft = true;
    hit.bp_id = hit_sb.id;
    hit.pc = hit_sb.addr;
    hit.lr = r.x[30];
    hit.sp = r.sp;
    hit.fp = r.x[29];
    hit.tid = tid;
    hit.type = BpType::Exec;
    std::snprintf(hit.msg, sizeof(hit.msg), "软断点#%d @0x%llX tid=%d%s",
                  hit_sb.id, (unsigned long long)hit_sb.addr, tid,
                  hit_sb.cond[0] ? " (cond)" : "");
    if (hit_sb.oneshot || hit_sb.id == g_temp_soft_id) {
      soft_bp_clear(hit_sb.id);
      if (hit_sb.id == g_temp_soft_id) {
        g_temp_soft_id = -1;
        g_temp_soft_addr = 0;
      }
      g_rearm_after_step = 0;
    } else {
      // 非 oneshot：保持 orig，resume 时单步后再插 BRK
      g_rearm_after_step = hit_sb.addr;
    }
    g_last_dbg_hit = hit;
    g_have_dbg_hit = true;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      g_hit_log.push_front(hit);
      while (g_hit_log.size() > kMaxLog) g_hit_log.pop_back();
    }
    set_st(hit.msg);
    return true;
  }

  // 其它停止（硬件/单步/手动）
  hit.valid = true;
  hit.pc = ur.pc;
  hit.lr = r.x[30];
  hit.sp = r.sp;
  hit.fp = r.x[29];
  hit.tid = tid;
  hit.is_soft = false;
  std::snprintf(hit.msg, sizeof(hit.msg), "停止 sig=%d PC=0x%llX tid=%d", sig,
                (unsigned long long)ur.pc, tid);
  g_last_dbg_hit = hit;
  g_have_dbg_hit = true;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_hit_log.push_front(hit);
    while (g_hit_log.size() > kMaxLog) g_hit_log.pop_back();
  }
  set_st(hit.msg);
  (void)sig;
  return true;
}

// ── 浮点寄存器 ────────────────────────────────────────────
// user_fpsimd_state: vregs[32] of __uint128 or 2x u64
struct user_fpsimd_state {
  uint64_t vregs[64];  // 32 * 128bit = 64 u64
  uint32_t fpsr;
  uint32_t fpcr;
};

bool dbg_fp_regs_read(FpRegs& out) {
  out = {};
  if (!ensure_ptrace()) return false;
  bool need_resume = false;
  if (!g_dbg_paused) {
    if (dbg_pause()) need_resume = true;
  }
  user_fpsimd_state fs{};
  iovec iv{&fs, sizeof(fs)};
  int tid = focus_tid();
  // 优先 NT_ARM_VFP / fpsimd
  long rc = ptrace(PTRACE_GETREGSET, tid, (void*)(uintptr_t)0x40e /*NT_ARM_SVE may fail*/,
                   &iv);
  (void)rc;
  iv.iov_base = &fs;
  iv.iov_len = sizeof(fs);
  // Linux NT_PRFPREG for aarch64 is fpsimd
  if (ptrace(PTRACE_GETREGSET, tid, (void*)(uintptr_t)NT_PRFPREG, &iv) != 0) {
    // try NT_ARM_VFP
    if (ptrace(PTRACE_GETREGSET, tid, (void*)(uintptr_t)NT_ARM_VFP, &iv) != 0) {
      set_st("读浮点寄存器失败");
      if (need_resume) dbg_resume();
      return false;
    }
  }
  for (int i = 0; i < 32; ++i) {
    out.v[i][0] = fs.vregs[i * 2];
    out.v[i][1] = fs.vregs[i * 2 + 1];
    std::memcpy(&out.d[i], &fs.vregs[i * 2], sizeof(double));
  }
  out.valid = true;
  if (need_resume) dbg_resume();
  return true;
}

// ── Trace ─────────────────────────────────────────────────
bool dbg_trace(int max_n, std::vector<TraceEntry>& out) {
  out.clear();
  if (max_n < 1) max_n = 1;
  if (max_n > 256) max_n = 256;
  if (!ensure_ptrace()) return false;
  if (!dbg_is_paused() && !dbg_pause()) return false;
  for (int i = 0; i < max_n; ++i) {
    Regs r;
    if (!dbg_regs_read(r)) break;
    TraceEntry e;
    e.pc = r.pc;
    read_mem(r.pc, &e.raw, 4);
    // 简单反汇编一行
    auto ins = disasm_at(r.pc, 1, {});
    if (!ins.empty())
      std::snprintf(e.text, sizeof(e.text), "%s %s", ins[0].mnem, ins[0].ops);
    else
      std::snprintf(e.text, sizeof(e.text), ".word %08X", e.raw);
    out.push_back(e);
    if (!dbg_step()) break;
  }
  set_st("Trace 完成");
  return !out.empty();
}

// ── 注入 / remote call ────────────────────────────────────
bool inject_code(uintptr_t addr, const uint8_t* code, size_t len) {
  if (!code || !len) return false;
  if (!write_mem(addr, code, len)) {
    set_st("inject 写入失败");
    return false;
  }
  set_st("代码已注入");
  return true;
}

bool remote_call(uintptr_t fn, const uint64_t* args, int nargs, uint64_t* ret) {
  if (!fn) {
    set_st("remote_call: fn=0");
    return false;
  }
  if (!ensure_ptrace()) return false;
  // 清理待重插状态，避免 resume 路径干扰
  if (g_rearm_after_step) {
    std::lock_guard<std::mutex> lk(g_mu);
    soft_bp_rearm_step_locked(true);
  }
  if (!dbg_is_paused() && !dbg_pause()) return false;
  Regs saved;
  if (!dbg_regs_read(saved)) return false;
  if (!saved.pc) {
    set_st("remote_call: PC 无效");
    return false;
  }
  Regs r = saved;
  if (nargs > 8) nargs = 8;
  for (int i = 0; i < nargs && args; ++i) r.x[i] = args[i];
  // 返回陷阱：oneshot 软断点在暂停点 PC；LR=trap，PC=fn
  // （W^X 下无法在匿名页执行，故复用已可执行代码页上的 PC）
  uintptr_t trap = saved.pc & ~3ull;
  // 若 trap 已有持久软断点，改用其下一指令，避免覆盖 orig
  {
    auto sbp = soft_bp_list();
    for (auto& s : sbp) {
      if (s.addr == trap && !s.oneshot) {
        trap = (saved.pc + 4) & ~3ull;
        break;
      }
    }
  }
  int tid_bp = soft_bp_set_cond(trap, nullptr, true);
  if (tid_bp < 0) {
    set_st("remote_call: 无法设返回断点");
    return false;
  }
  g_temp_soft_id = tid_bp;
  g_temp_soft_addr = trap;
  r.x[30] = trap;  // lr
  r.pc = fn;
  r.valid = true;
  if (!dbg_regs_write(r)) {
    soft_bp_clear(tid_bp);
    g_temp_soft_id = -1;
    return false;
  }
  if (!dbg_resume()) {
    soft_bp_clear(tid_bp);
    g_temp_soft_id = -1;
    dbg_regs_write(saved);
    return false;
  }
  // 等待命中（最多 ~3s）
  for (int i = 0; i < 300; ++i) {
    BpHit h;
    if (dbg_poll_stop(h)) {
      // 确认停在返回点附近
      bool at_trap = (h.pc == trap) || (h.is_soft && h.bp_id == tid_bp);
      if (!at_trap && h.pc) {
        // 其它信号/断点：继续等
        if (h.pc != trap) {
          // 若已暂停在别处，继续跑直到超时
          dbg_resume();
          usleep(10000);
          continue;
        }
      }
      Regs rr;
      if (!dbg_regs_read(rr)) rr = {};
      if (ret) *ret = rr.x[0];
      soft_bp_clear(tid_bp);
      g_temp_soft_id = -1;
      g_temp_soft_addr = 0;
      g_rearm_after_step = 0;
      dbg_regs_write(saved);
      set_st("remote_call 完成");
      return true;
    }
    usleep(10000);
  }
  soft_bp_clear(tid_bp);
  g_temp_soft_id = -1;
  g_temp_soft_addr = 0;
  dbg_pause();
  dbg_regs_write(saved);
  set_st("remote_call 超时");
  return false;
}

// 解析到的 dlopen 形态（决定 remote_call 参数个数）
enum class DlopenKind { Std2, Loader3 };
static DlopenKind g_dlopen_kind = DlopenKind::Std2;
static char g_dlopen_name[32] = "dlopen";

bool find_dlopen(uintptr_t& out_addr) {
  out_addr = 0;
  if (!is_attached()) {
    set_st("find_dlopen: 未附加");
    return false;
  }
  if (sym_count() == 0) sym_refresh();
  char mod[128]{};
  // 优先标准 2 参 dlopen（libdl / libc），避免 __loader_dlopen 错误调用约定崩进程
  struct Cand {
    const char* name;
    DlopenKind kind;
    int score_bonus;
  };
  static const Cand kCands[] = {
      {"dlopen", DlopenKind::Std2, 30},
      {"__dl_dlopen", DlopenKind::Std2, 20},
      {"android_dlopen_ext", DlopenKind::Loader3, 10},
      {"__loader_dlopen", DlopenKind::Loader3, 0},
      {nullptr, DlopenKind::Std2, 0},
  };
  auto pick = [&](bool force_refresh) -> bool {
    if (force_refresh) sym_refresh();
    uintptr_t best_a = 0;
    int best_sc = -1;
    DlopenKind best_k = DlopenKind::Std2;
    const char* best_n = "dlopen";
    char best_mod[128]{};
    for (int i = 0; kCands[i].name; ++i) {
      uintptr_t a = 0;
      char m[128]{};
      if (!sym_find_by_name(kCands[i].name, a, m, sizeof(m)) || !a) continue;
      int sc = kCands[i].score_bonus;
      // 偏好 libdl / linker 上的符号
      if (std::strstr(m, "libdl")) sc += 40;
      else if (std::strstr(m, "linker")) sc += 15;
      else if (std::strstr(m, "libc.so")) sc += 25;
      // 标准 dlopen 额外加分
      if (kCands[i].kind == DlopenKind::Std2) sc += 15;
      if (sc > best_sc) {
        best_sc = sc;
        best_a = a;
        best_k = kCands[i].kind;
        best_n = kCands[i].name;
        std::snprintf(best_mod, sizeof(best_mod), "%s", m);
      }
    }
    if (!best_a) return false;
    out_addr = best_a;
    g_dlopen_kind = best_k;
    std::snprintf(g_dlopen_name, sizeof(g_dlopen_name), "%s", best_n);
    char b[140];
    std::snprintf(b, sizeof(b), "dlopen=%s @0x%llx (%s) args=%s", best_n,
                  (unsigned long long)best_a, best_mod,
                  best_k == DlopenKind::Std2 ? "2" : "3");
    set_st(b);
    return true;
  };
  if (pick(false)) return true;
  if (pick(true)) return true;
  set_st("未找到 dlopen 符号（检查 SELinux/是否附加）");
  return false;
}

bool inject_so(uintptr_t dlopen_addr, const char* path, uint64_t* handle_out) {
  if (!path || !path[0]) {
    set_st("inject_so: 需要 so 路径");
    return false;
  }
  if (!dlopen_addr) {
    if (!find_dlopen(dlopen_addr)) return false;
  } else {
    // 用户手填地址：默认按 2 参；若此前 find 过 loader 则沿用
    // 无法可靠探测，优先 Std2
  }
  // path 写入目标：优先大块匿名可写尾部
  auto maps = load_maps(true);
  uintptr_t slot = 0;
  size_t len = std::strlen(path) + 1;
  if (len > 512) len = 512;
  uint8_t backup[512]{};
  bool have_backup = false;
  auto try_slot = [&](uintptr_t cand) -> bool {
    if (!cand) return false;
    if (!read_mem(cand, backup, len)) return false;
    if (!write_mem(cand, path, len)) return false;
    char check[512]{};
    if (!read_mem(cand, check, len) || std::memcmp(check, path, len) != 0) {
      write_mem(cand, backup, len);
      return false;
    }
    slot = cand;
    have_backup = true;
    return true;
  };
  for (auto it = maps.rbegin(); it != maps.rend(); ++it) {
    auto& r = *it;
    if (!r.writable || r.end - r.start < 0x2000 || r.start < 0x10000) continue;
    bool anon = r.path.empty() || r.path[0] == '[';
    if (!anon) continue;
    // 避开 [stack]
    if (r.path.find("stack") != std::string::npos) continue;
    if (try_slot(r.end - 0x800)) break;
  }
  if (!slot) {
    for (auto it = maps.rbegin(); it != maps.rend(); ++it) {
      auto& r = *it;
      if (!r.writable || r.end - r.start < 0x1000 || r.start < 0x10000) continue;
      if (r.path.find("stack") != std::string::npos) continue;
      if (try_slot(r.end - 0x400)) break;
    }
  }
  if (!slot) {
    set_st("inject_so: 无写入槽");
    return false;
  }

  // 取一个合法 caller 地址（libc 代码页），供 __loader_dlopen 使用
  uintptr_t caller = 0;
  {
    auto all = load_maps(false);
    for (auto& r : all) {
      if (r.perms[2] != 'x') continue;
      if (r.path.find("libc.so") != std::string::npos ||
          r.path.find("libdl.so") != std::string::npos) {
        caller = (r.start + 0x1000) & ~3ull;
        break;
      }
    }
    if (!caller) {
      for (auto& r : all) {
        if (r.perms[2] == 'x' && r.end - r.start > 0x2000) {
          caller = (r.start + 0x1000) & ~3ull;
          break;
        }
      }
    }
  }

  // RTLD_NOW = 2
  uint64_t h = 0;
  bool ok = false;
  if (g_dlopen_kind == DlopenKind::Loader3) {
    // __loader_dlopen(path, flags, caller_addr)
    uint64_t args3[3] = {slot, 2, caller ? caller : slot};
    ok = remote_call(dlopen_addr, args3, 3, &h);
  } else {
    // dlopen(path, flags)
    uint64_t args2[2] = {slot, 2};
    ok = remote_call(dlopen_addr, args2, 2, &h);
    // 若失败再试 loader 3 参（用户手填了 loader 地址）
    if ((!ok || h == 0) && caller) {
      uint64_t args3[3] = {slot, 2, caller};
      uint64_t h2 = 0;
      if (remote_call(dlopen_addr, args3, 3, &h2)) {
        ok = true;
        h = h2;
        g_dlopen_kind = DlopenKind::Loader3;
      }
    }
  }

  // 恢复 path 槽
  if (have_backup) write_mem(slot, backup, len);

  if (!ok) {
    set_st("inject_so: remote_call 失败");
    return false;
  }
  if (handle_out) *handle_out = h;
  char b[120];
  if (h == 0) {
    std::snprintf(b, sizeof(b), "dlopen 返回 NULL（路径无效或被拒绝）");
    set_st(b);
    return false;
  }
  std::snprintf(b, sizeof(b), "dlopen(%s) -> 0x%llx", g_dlopen_name,
                (unsigned long long)h);
  set_st(b);
  return true;
}

int bp_set_cond(uintptr_t addr, BpType type, int size, const char* cond) {
  int id = bp_set(addr, type, size);
  if (id >= 0 && cond && cond[0]) bp_set_condition(id, cond);
  return id;
}

bool bp_set_condition(int id, const char* cond) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (auto& b : g_perf) {
    if (b.info.id == id) {
      if (cond)
        std::snprintf(b.info.cond, sizeof(b.info.cond), "%s", cond);
      else
        b.info.cond[0] = 0;
      return true;
    }
  }
  for (auto& b : g_ptrace_bps) {
    if (b.id == id) {
      if (cond)
        std::snprintf(b.cond, sizeof(b.cond), "%s", cond);
      else
        b.cond[0] = 0;
      return true;
    }
  }
  for (auto& s : g_soft) {
    if (s.id == id) {
      if (cond)
        std::snprintf(s.cond, sizeof(s.cond), "%s", cond);
      else
        s.cond[0] = 0;
      return true;
    }
  }
  return false;
}

}  // namespace mem


