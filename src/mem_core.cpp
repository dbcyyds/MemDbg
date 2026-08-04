#include "mem_core.hpp"
#include "mem_icon.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace mem {
namespace {

constexpr size_t kChunk = 256 * 1024;
constexpr size_t kMaxStore = 80000;   // 内存中最多保留
constexpr size_t kMaxUnknown = 200000;

std::mutex g_mu;
int g_pid = -1;
std::string g_name;
int g_mem_fd = -1;

std::vector<Match> g_results;
int g_round = 0;
ValType g_last_type = ValType::I32;

std::atomic<bool> g_busy{false};
std::atomic<float> g_progress{0.f};
std::atomic<bool> g_cancel{false};
char g_status[128] = "就绪";
std::thread g_worker;

// 冻结表
struct FreezeItem {
  uintptr_t addr = 0;
  uint64_t bits = 0;
  size_t size = 4;
};
std::vector<FreezeItem> g_freeze;

void set_status(const char* s) {
  std::snprintf(g_status, sizeof(g_status), "%s", s ? s : "");
}

std::string read_file_str(const char* path, size_t maxn = 512) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return {};
  std::string s;
  s.resize(maxn);
  size_t n = std::fread(s.data(), 1, maxn, f);
  std::fclose(f);
  s.resize(n);
  // cmdline 用 \0 分隔
  for (char& c : s)
    if (c == '\0') c = ' ';
  while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r'))
    s.pop_back();
  return s;
}

bool open_mem_fd() {
  if (g_mem_fd >= 0) {
    close(g_mem_fd);
    g_mem_fd = -1;
  }
  if (g_pid <= 0) return false;
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/%d/mem", g_pid);
  g_mem_fd = open(path, O_RDWR | O_CLOEXEC);
  if (g_mem_fd < 0) g_mem_fd = open(path, O_RDONLY | O_CLOEXEC);
  return g_mem_fd >= 0;
}

bool read_via_vm(uintptr_t addr, void* buf, size_t len) {
  if (g_pid <= 0 || !buf || len == 0) return false;
  struct iovec local{buf, len};
  struct iovec remote{reinterpret_cast<void*>(addr), len};
  ssize_t n = process_vm_readv(g_pid, &local, 1, &remote, 1, 0);
  return n == (ssize_t)len;
}

bool write_via_vm(uintptr_t addr, const void* buf, size_t len) {
  if (g_pid <= 0 || !buf || len == 0) return false;
  struct iovec local{const_cast<void*>(buf), len};
  struct iovec remote{reinterpret_cast<void*>(addr), len};
  ssize_t n = process_vm_writev(g_pid, &local, 1, &remote, 1, 0);
  return n == (ssize_t)len;
}

bool read_via_fd(uintptr_t addr, void* buf, size_t len) {
  if (g_mem_fd < 0 || !buf || len == 0) return false;
  if (lseek64(g_mem_fd, (off64_t)addr, SEEK_SET) < 0) return false;
  size_t got = 0;
  auto* p = static_cast<uint8_t*>(buf);
  while (got < len) {
    ssize_t n = read(g_mem_fd, p + got, len - got);
    if (n <= 0) return false;
    got += (size_t)n;
  }
  return true;
}

bool write_via_fd(uintptr_t addr, const void* buf, size_t len) {
  if (g_mem_fd < 0 || !buf || len == 0) return false;
  if (lseek64(g_mem_fd, (off64_t)addr, SEEK_SET) < 0) return false;
  size_t put = 0;
  auto* p = static_cast<const uint8_t*>(buf);
  while (put < len) {
    ssize_t n = write(g_mem_fd, p + put, len - put);
    if (n <= 0) return false;
    put += (size_t)n;
  }
  return true;
}

bool join_worker() {
  if (g_worker.joinable()) {
    g_worker.join();
    return true;
  }
  return false;
}

double bits_to_f(ValType t, uint64_t bits) {
  if (t == ValType::F32) {
    float f;
    uint32_t u = (uint32_t)bits;
    std::memcpy(&f, &u, 4);
    return (double)f;
  }
  if (t == ValType::F64) {
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
  }
  // 整数按有符号
  switch (t) {
    case ValType::I8:
      return (double)(int8_t)bits;
    case ValType::I16:
      return (double)(int16_t)bits;
    case ValType::I32:
      return (double)(int32_t)bits;
    case ValType::I64:
      return (double)(int64_t)bits;
    default:
      return (double)bits;
  }
}

uint64_t load_bits(const uint8_t* p, size_t sz) {
  uint64_t v = 0;
  std::memcpy(&v, p, sz);
  return v;
}

bool match_compare(const ScanConfig& cfg, uint64_t cur, uint64_t prev,
                   bool has_prev) {
  if (cfg.mode == ScanMode::Unknown) return true;

  if (cfg.type == ValType::Hex) {
    // Hex 在扫描循环中单独处理
    return true;
  }

  if (cfg.is_float) {
    double c = bits_to_f(cfg.type, cur);
    double a = cfg.a_f, b = cfg.b_f;
    switch (cfg.mode) {
      case ScanMode::Exact:
        return std::fabs(c - a) < 1e-5;
      case ScanMode::Fuzzy:
        return std::fabs(c - a) <= (cfg.fuzzy_tol > 0 ? cfg.fuzzy_tol : 1e-3);
      case ScanMode::Greater:
        return c > a;
      case ScanMode::Less:
        return c < a;
      case ScanMode::Between:
        return c >= a && c <= b;
      case ScanMode::Changed:
        return has_prev && std::fabs(c - bits_to_f(cfg.type, prev)) > 1e-6;
      case ScanMode::Unchanged:
        return has_prev && std::fabs(c - bits_to_f(cfg.type, prev)) <= 1e-6;
      case ScanMode::Increased:
        return has_prev && c > bits_to_f(cfg.type, prev) + 1e-6;
      case ScanMode::Decreased:
        return has_prev && c < bits_to_f(cfg.type, prev) - 1e-6;
      default:
        return false;
    }
  }

  int64_t c = 0, a = 0, b = 0, p = 0;
  switch (cfg.type_size) {
    case 1:
      c = (int8_t)cur;
      a = (int8_t)cfg.a_bits;
      b = (int8_t)cfg.b_bits;
      p = (int8_t)prev;
      break;
    case 2:
      c = (int16_t)cur;
      a = (int16_t)cfg.a_bits;
      b = (int16_t)cfg.b_bits;
      p = (int16_t)prev;
      break;
    case 4:
      c = (int32_t)cur;
      a = (int32_t)cfg.a_bits;
      b = (int32_t)cfg.b_bits;
      p = (int32_t)prev;
      break;
    default:
      c = (int64_t)cur;
      a = (int64_t)cfg.a_bits;
      b = (int64_t)cfg.b_bits;
      p = (int64_t)prev;
      break;
  }

  switch (cfg.mode) {
    case ScanMode::Exact:
      return c == a;
    case ScanMode::Fuzzy: {
      int64_t tol = (int64_t)(cfg.fuzzy_tol > 0 ? cfg.fuzzy_tol : 1.0);
      if (tol < 0) tol = -tol;
      int64_t d = c - a;
      if (d < 0) d = -d;
      return d <= tol;
    }
    case ScanMode::Greater:
      return c > a;
    case ScanMode::Less:
      return c < a;
    case ScanMode::Between:
      return c >= a && c <= b;
    case ScanMode::Changed:
      return has_prev && c != p;
    case ScanMode::Unchanged:
      return has_prev && c == p;
    case ScanMode::Increased:
      return has_prev && c > p;
    case ScanMode::Decreased:
      return has_prev && c < p;
    default:
      return false;
  }
}

inline bool is_pat_type(ValType t) {
  return t == ValType::Hex || t == ValType::StrUtf8 || t == ValType::StrUtf16;
}

inline uint8_t ascii_fold(uint8_t c) {
  if (c >= 'A' && c <= 'Z') return (uint8_t)(c + 32);
  return c;
}

/** 模式匹配（Hex 掩码 / 字符串；可选 ASCII 大小写不敏感） */
bool match_pattern_at(const uint8_t* mem, size_t plen,
                      const std::vector<uint8_t>& pat,
                      const std::vector<uint8_t>& mask, bool case_ins,
                      bool utf16) {
  if (plen == 0 || pat.size() != plen) return false;
  const bool has_mask = mask.size() == plen;
  if (!utf16) {
    for (size_t k = 0; k < plen; ++k) {
      uint8_t msk = has_mask ? mask[k] : (uint8_t)0xFF;
      if (msk == 0) continue;
      uint8_t a = mem[k] & msk;
      uint8_t b = pat[k] & msk;
      if (case_ins && msk == 0xFF) {
        a = ascii_fold(a);
        b = ascii_fold(b);
      }
      if (a != b) return false;
    }
    return true;
  }
  // UTF-16LE：按 2 字节单元，仅低字节 ASCII 可折大小写
  for (size_t k = 0; k + 1 < plen; k += 2) {
    uint8_t lo = mem[k], hi = mem[k + 1];
    uint8_t plo = pat[k], phi = pat[k + 1];
    if (has_mask) {
      if (mask[k] == 0 && mask[k + 1] == 0) continue;
      lo = (uint8_t)(lo & mask[k]);
      hi = (uint8_t)(hi & mask[k + 1]);
      plo = (uint8_t)(plo & mask[k]);
      phi = (uint8_t)(phi & mask[k + 1]);
    }
    if (case_ins && hi == 0 && phi == 0) {
      lo = ascii_fold(lo);
      plo = ascii_fold(plo);
    }
    if (lo != plo || hi != phi) return false;
  }
  if (plen & 1) {
    // 奇数长度极少见
    size_t k = plen - 1;
    uint8_t msk = has_mask ? mask[k] : (uint8_t)0xFF;
    if (msk && ((mem[k] & msk) != (pat[k] & msk))) return false;
  }
  return true;
}

void scan_regions_first(const ScanConfig& cfg, std::vector<Match>& out) {
  // load_maps_filtered 在下方 mem 命名空间实现
  auto regions = ::mem::load_maps_filtered(cfg.region);
  if (regions.empty()) regions = load_maps(true);
  if (regions.empty()) regions = load_maps(false);

  size_t total = 0;
  for (auto& r : regions) total += (r.end - r.start);
  if (total == 0) total = 1;

  size_t done = 0;
  // 跨 chunk 边界：多读 overlap
  const size_t plen_hint = cfg.hex_pat.empty() ? 0 : cfg.hex_pat.size();
  std::vector<uint8_t> buf(kChunk + plen_hint + 16);
  const bool pattern = is_pat_type(cfg.type);
  const size_t step =
      pattern ? 1 : (cfg.type_size > 0 ? cfg.type_size : 4);
  // UTF-16 可按 2 对齐加速（可选）；默认 1 以免漏非对齐串
  const size_t align = (cfg.type == ValType::StrUtf16) ? 2 : step;

  for (size_t ri = 0; ri < regions.size() && !g_cancel.load(); ++ri) {
    auto& reg = regions[ri];
    uintptr_t addr = reg.start;
    if (align > 1) addr = (addr + align - 1) & ~(uintptr_t)(align - 1);

    while (addr < reg.end && !g_cancel.load()) {
      size_t want = std::min((size_t)(reg.end - addr), kChunk);
      size_t need = want;
      if (pattern && plen_hint > 1)
        need = std::min((size_t)(reg.end - addr), want + plen_hint - 1);
      if (!pattern && want < cfg.type_size) break;
      if (pattern && plen_hint && need < plen_hint) {
        addr += want ? want : 0x1000;
        done += want;
        continue;
      }
      if (!read_mem(addr, buf.data(), need)) {
        addr += want ? want : 0x1000;
        done += want;
        g_progress.store((float)done / (float)total);
        continue;
      }

      if (pattern) {
        const size_t plen = cfg.hex_pat.size();
        if (plen == 0 || need < plen) {
          addr += want;
          done += want;
          continue;
        }
        // 只在 [0, want) 起点扫描，边界用 overlap
        const size_t lim = want;
        const bool utf16 = cfg.type == ValType::StrUtf16;
        for (size_t i = 0; i < lim && i + plen <= need; i += step) {
          if (match_pattern_at(buf.data() + i, plen, cfg.hex_pat, cfg.hex_mask,
                               cfg.str_case_insensitive, utf16)) {
            Match m;
            m.addr = addr + i;
            m.value_bits = 0;
            m.prev_bits = 0;
            m.match_len = plen;
            // 预览前 8 字节
            size_t pv = plen < 8 ? plen : 8;
            std::memcpy(&m.value_bits, buf.data() + i, pv);
            out.push_back(m);
            if (out.size() >= kMaxStore) return;
          }
        }
      } else {
        size_t i = 0;
        const size_t lim = want - cfg.type_size + 1;
        for (; i < lim; i += step) {
          uint64_t cur = load_bits(buf.data() + i, cfg.type_size);
          if (cfg.mode == ScanMode::Unknown ||
              match_compare(cfg, cur, 0, false)) {
            Match m;
            m.addr = addr + i;
            m.value_bits = cur;
            m.prev_bits = cur;
            m.match_len = cfg.type_size;
            out.push_back(m);
            if (out.size() >=
                (cfg.mode == ScanMode::Unknown ? kMaxUnknown : kMaxStore))
              return;
          }
        }
      }

      addr += want;
      done += want;
      g_progress.store(std::min(0.99f, (float)done / (float)total));
    }
  }
  g_progress.store(1.f);
}

void scan_filter_next(const ScanConfig& cfg, std::vector<Match>& io) {
  std::vector<Match> next;
  next.reserve(io.size() / 2 + 8);
  const size_t n = io.size();
  const bool pattern = is_pat_type(cfg.type);
  std::vector<uint8_t> tmp;
  for (size_t i = 0; i < n && !g_cancel.load(); ++i) {
    auto& m = io[i];
    size_t sz = pattern ? cfg.hex_pat.size() : cfg.type_size;
    if (sz == 0) sz = 4;
    if (sz > 512) sz = 512;
    tmp.resize(sz);
    if (!read_mem(m.addr, tmp.data(), sz)) continue;

    if (pattern) {
      const size_t plen = cfg.hex_pat.size();
      bool ok = match_pattern_at(tmp.data(), plen, cfg.hex_pat, cfg.hex_mask,
                                 cfg.str_case_insensitive,
                                 cfg.type == ValType::StrUtf16);
      if (ok) {
        m.prev_bits = m.value_bits;
        m.match_len = plen;
        size_t pv = plen < 8 ? plen : 8;
        m.value_bits = 0;
        std::memcpy(&m.value_bits, tmp.data(), pv);
        next.push_back(m);
      }
    } else {
      uint64_t cur = load_bits(tmp.data(), cfg.type_size);
      bool ok = match_compare(cfg, cur, m.value_bits, true);
      if (cfg.mode == ScanMode::Exact || cfg.mode == ScanMode::Greater ||
          cfg.mode == ScanMode::Less || cfg.mode == ScanMode::Between ||
          cfg.mode == ScanMode::Fuzzy) {
        ok = match_compare(cfg, cur, 0, false);
      }
      if (ok) {
        m.prev_bits = m.value_bits;
        m.value_bits = cur;
        next.push_back(m);
      }
    }
    if ((i & 0x3FF) == 0)
      g_progress.store((float)i / (float)std::max<size_t>(n, 1));
  }
  io.swap(next);
  g_progress.store(1.f);
}

}  // namespace

size_t type_size_of(ValType t) {
  switch (t) {
    case ValType::I8:
      return 1;
    case ValType::I16:
      return 2;
    case ValType::I32:
    case ValType::F32:
      return 4;
    case ValType::I64:
    case ValType::F64:
      return 8;
    case ValType::Hex:
    case ValType::StrUtf8:
      return 1;
    case ValType::StrUtf16:
      return 2;
    default:
      return 4;
  }
}

bool is_pattern_type(ValType t) {
  return t == ValType::Hex || t == ValType::StrUtf8 || t == ValType::StrUtf16;
}

bool is_system_process(int pid, const std::string& name) {
  if (name.empty()) return true;
  // 内核线程 [kworker/...]
  if (name[0] == '[') return true;

  static const char* kSysExact[] = {
      "init",           "zygote",          "zygote64",
      "system_server",  "surfaceflinger",  "servicemanager",
      "hwservicemanager","vndservicemanager","audioserver",
      "cameraserver",   "drmserver",       "installd",
      "keystore2",      "logd",            "lmkd",
      "netd",           "storaged",        "vold",
      "ueventd",        "adbd",            "tombstoned",
      "statsd",         "incidentd",       "traced",
      "traced_probes",  "webview_zygote",  "usap32",
      "usap64",         "idmap2d",         "gpuservice",
      "update_engine",  "wificond",        "prf",
      "app_process",    "app_process32",   "app_process64",
      nullptr};
  for (int i = 0; kSysExact[i]; ++i)
    if (name == kSysExact[i]) return true;

  static const char* kSysPref[] = {
      "zygote", "com.android.", "android.process.", "system@",
      "/system/", "/apex/", "/vendor/", "prng_seeder", nullptr};
  for (int i = 0; kSysPref[i]; ++i)
    if (name.rfind(kSysPref[i], 0) == 0) return true;

  // 包名形态（com.xxx / org.xxx）：普通 App，绝不能因 exe=app_process 判系统
  // Android 所有 Java/Kotlin App 的 /proc/pid/exe 都是 /system/bin/app_process*
  if (name.find('.') != std::string::npos) {
    if (name.rfind("com.android.", 0) == 0) return true;
    if (name.rfind("com.google.android.", 0) == 0) return true;
    // 仍视为用户 App（含 com.tencent.mobileqq 等）
    return false;
  }

  // native 进程：exe 落在 system/apex/vendor 且不是 app_process
  char link[128], target[256]{};
  std::snprintf(link, sizeof(link), "/proc/%d/exe", pid);
  ssize_t n = readlink(link, target, sizeof(target) - 1);
  if (n > 0) {
    target[n] = 0;
    // app_process* 是 App 宿主，不能据此判系统（name 无点号时才走到这）
    bool is_app_process =
        std::strstr(target, "app_process") != nullptr;
    if (!is_app_process &&
        (std::strncmp(target, "/system/", 8) == 0 ||
         std::strncmp(target, "/apex/", 6) == 0 ||
         std::strncmp(target, "/vendor/", 8) == 0))
      return true;
  }

  // 读 Uid：系统 UID < 10000（app 一般 >= 10000）
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/%d/status", pid);
  FILE* f = std::fopen(path, "r");
  if (f) {
    char line[128];
    while (std::fgets(line, sizeof(line), f)) {
      if (std::strncmp(line, "Uid:", 4) == 0) {
        int uid = 0;
        std::sscanf(line + 4, "%d", &uid);
        std::fclose(f);
        if (uid > 0 && uid < 10000) return true;
        // app uid (>=10000) 即使 name 无点号也保留
        if (uid >= 10000) return false;
        break;
      }
    }
    if (f) std::fclose(f);
  }
  return false;
}

bool is_tencent_package(const std::string& name) {
  if (name.empty()) return false;
  // 常见腾讯包名 / 进程
  if (name.rfind("com.tencent.", 0) == 0) return true;
  if (name.find("tencent") != std::string::npos) return true;
  if (name.find("Tencent") != std::string::npos) return true;
  // 微信 / QQ / 王者 / 和平 / 元梦 等
  static const char* kKeys[] = {
      "com.tencent.mm",       "com.tencent.mobileqq", "com.tencent.tmgp",
      "com.tencent.wework",   "com.tencent.qqmusic",  "com.tencent.qqlive",
      "com.tencent.tim",      "com.tencent.wetype",   "com.tencent.qqpinyin",
      "com.tencent.androidqqmail", "com.tencent.mtt",  "com.tencent.news",
      "com.tencent.map",      "com.tencent.weread",   "com.tencent.game",
      "com.tencent.lolm",     "com.tencent.nfsonline", nullptr};
  for (int i = 0; kKeys[i]; ++i)
    if (name.rfind(kKeys[i], 0) == 0 || name.find(kKeys[i]) != std::string::npos)
      return true;
  return false;
}

bool process_has_app_icon(int pid, const std::string& name) {
  // 有应用图标 ≈ 可对应到 Android 包（packages.list / APK / 数据目录）
  if (name.find('.') == std::string::npos) return false;
  std::string pkg = name;
  auto sp = pkg.find(' ');
  if (sp != std::string::npos) pkg = pkg.substr(0, sp);
  auto colon = pkg.find(':');  // com.xxx:push
  if (colon != std::string::npos) pkg = pkg.substr(0, colon);
  while (!pkg.empty() &&
         (pkg.back() == ' ' || pkg.back() == '\t' || pkg.back() == '\0'))
    pkg.pop_back();
  while (!pkg.empty() && (pkg.front() == ' ' || pkg.front() == '\t'))
    pkg.erase(pkg.begin());

  // packages.list / apk 索引（root 下最准）
  if (package_is_app(pkg)) return true;

  char path[288];
  std::snprintf(path, sizeof(path), "/data/data/%s", pkg.c_str());
  if (access(path, F_OK) == 0) return true;
  std::snprintf(path, sizeof(path), "/data/user/0/%s", pkg.c_str());
  if (access(path, F_OK) == 0) return true;
  std::snprintf(path, sizeof(path), "/data/user_de/0/%s", pkg.c_str());
  if (access(path, F_OK) == 0) return true;

  char stpath[64];
  std::snprintf(stpath, sizeof(stpath), "/proc/%d/status", pid);
  FILE* sf = std::fopen(stpath, "r");
  if (sf) {
    char line[128];
    int uid = -1;
    while (std::fgets(line, sizeof(line), sf)) {
      if (std::strncmp(line, "Uid:", 4) == 0) {
        std::sscanf(line + 4, "%d", &uid);
        break;
      }
    }
    std::fclose(sf);
    if (uid >= 10000) {
      int user = uid / 100000;
      std::snprintf(path, sizeof(path), "/data/user/%d/%s", user, pkg.c_str());
      if (access(path, F_OK) == 0) return true;
      // app uid + 包名形态（含 android./bin. 等）
      if (pkg.rfind("com.", 0) == 0 || pkg.rfind("org.", 0) == 0 ||
          pkg.rfind("net.", 0) == 0 || pkg.rfind("io.", 0) == 0 ||
          pkg.rfind("cn.", 0) == 0 || pkg.rfind("tv.", 0) == 0 ||
          pkg.rfind("android.", 0) == 0 || pkg.rfind("bin.", 0) == 0 ||
          pkg.rfind("vendor.", 0) == 0)
        return true;
    }
  }

  if (pkg.rfind("com.", 0) == 0 || pkg.rfind("org.", 0) == 0 ||
      pkg.rfind("net.", 0) == 0 || pkg.rfind("io.", 0) == 0 ||
      pkg.rfind("android.", 0) == 0 || pkg.rfind("bin.", 0) == 0)
    return true;
  return false;
}

static int hash_icon_color(const std::string& s) {
  unsigned h = 2166136261u;
  for (unsigned char c : s) {
    h ^= c;
    h *= 16777619u;
  }
  // 粉彩友好色相
  return (int)(h & 0xFFFFFF);
}

std::vector<ProcInfo> list_processes(const ProcListOptions& opt) {
  std::vector<ProcInfo> out;
  DIR* d = opendir("/proc");
  if (!d) return out;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    int pid = atoi(e->d_name);
    if (pid <= 0) continue;
    char path[128];
    std::snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    std::string name = read_file_str(path, 256);
    if (name.empty()) {
      std::snprintf(path, sizeof(path), "/proc/%d/comm", pid);
      name = read_file_str(path, 64);
    }
    if (name.empty()) continue;
    auto sp = name.find(' ');
    std::string show = sp == std::string::npos ? name : name.substr(0, sp);
    auto colon = show.find(':');
    std::string pkg = colon != std::string::npos ? show.substr(0, colon) : show;

    if (opt.skip_system && is_system_process(pid, show)) continue;

    bool has_icon = process_has_app_icon(pid, show);
    if (opt.skip_no_icon && !has_icon) continue;

    bool tencent = is_tencent_package(show) || is_tencent_package(pkg);
    if (opt.tencent_only && !tencent) continue;

    if (opt.filter && opt.filter[0]) {
      std::string pid_s = std::to_string(pid);
      bool hit = false;
      if (opt.fuzzy_filter) {
        hit = fuzzy_match(show.c_str(), opt.filter) ||
              fuzzy_match(name.c_str(), opt.filter) ||
              fuzzy_match(pkg.c_str(), opt.filter) ||
              fuzzy_match(pid_s.c_str(), opt.filter);
      } else {
        hit = show.find(opt.filter) != std::string::npos ||
              name.find(opt.filter) != std::string::npos ||
              pid_s.find(opt.filter) != std::string::npos;
      }
      if (!hit) continue;
    }

    ProcInfo pi;
    pi.pid = pid;
    pi.name = show;
    pi.package = pkg;
    pi.has_icon = has_icon;
    pi.is_tencent = tencent;
    pi.icon_color = hash_icon_color(pkg);
    out.push_back(std::move(pi));
  }
  closedir(d);
  std::sort(out.begin(), out.end(),
            [](const ProcInfo& a, const ProcInfo& b) { return a.pid < b.pid; });
  return out;
}

std::vector<ProcInfo> list_processes(const char* filter, bool skip_system) {
  ProcListOptions opt;
  opt.filter = filter;
  opt.skip_system = skip_system;
  opt.skip_no_icon = false;
  opt.tencent_only = false;
  return list_processes(opt);
}

bool attach(int pid) {
  detach();
  if (pid <= 0) return false;
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/%d", pid);
  if (access(path, F_OK) != 0) return false;

  g_pid = pid;
  std::snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
  g_name = read_file_str(path, 256);
  if (g_name.empty()) {
    std::snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    g_name = read_file_str(path, 64);
  }
  auto sp = g_name.find(' ');
  if (sp != std::string::npos) g_name = g_name.substr(0, sp);

  open_mem_fd();

  // 尝试读 maps 中第一段可读内存
  auto maps = load_maps(false);
  bool can_read = false;
  for (auto& r : maps) {
    uint8_t b = 0;
    if (read_mem(r.start, &b, 1)) {
      can_read = true;
      break;
    }
  }
  if (!can_read && maps.empty()) {
    // 无 maps 也可能是权限问题
    set_status("附加失败：无法读取 /proc/pid/maps（需要 root）");
    detach();
    return false;
  }
  if (!can_read) {
    set_status("附加失败：无法读取目标内存（需要 root）");
    detach();
    return false;
  }
  set_status("已附加");
  return true;
}

void detach() {
  join_worker();
  g_busy = false;
  // 断点模块在 float_app 退出/断开时也会调 bp_detach；这里避免循环依赖，仅清内存侧
  if (g_mem_fd >= 0) {
    close(g_mem_fd);
    g_mem_fd = -1;
  }
  g_pid = -1;
  g_name.clear();
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_results.clear();
    g_freeze.clear();
  }
  g_round = 0;
  set_status("已断开");
}

bool is_attached() { return g_pid > 0; }
int attached_pid() { return g_pid; }
const char* attached_name() { return g_name.c_str(); }

bool read_mem(uintptr_t addr, void* buf, size_t len) {
  if (g_pid <= 0) return false;
  if (read_via_vm(addr, buf, len)) return true;
  return read_via_fd(addr, buf, len);
}

bool write_mem(uintptr_t addr, const void* buf, size_t len) {
  if (g_pid <= 0) return false;
  if (write_via_vm(addr, buf, len)) return true;
  // 只读打开时无法写
  if (g_mem_fd >= 0) {
    // 尝试以 RDWR 重开
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/mem", g_pid);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
      if (g_mem_fd >= 0) close(g_mem_fd);
      g_mem_fd = fd;
    }
  }
  return write_via_fd(addr, buf, len);
}

std::vector<Region> load_maps(bool writable_only) {
  std::vector<Region> out;
  if (g_pid <= 0) return out;
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/%d/maps", g_pid);
  FILE* f = std::fopen(path, "r");
  if (!f) return out;
  char line[512];
  while (std::fgets(line, sizeof(line), f)) {
    uintptr_t start = 0, end = 0;
    char perms[8]{};
    // 7fffffffff000-7fffffffff000 rw-p 00000000 00:00 0  [anon:...]
    int n = std::sscanf(line, "%lx-%lx %4s", &start, &end, perms);
    if (n < 3 || end <= start) continue;
    Region r;
    r.start = start;
    r.end = end;
    std::memcpy(r.perms, perms, 4);
    r.perms[4] = 0;
    r.readable = perms[0] == 'r';
    r.writable = perms[1] == 'w';
    if (!r.readable) continue;
    if (writable_only && !r.writable) continue;
    // path
    const char* p = line;
    // skip to path after 5th field
    int fields = 0;
    while (*p && fields < 5) {
      if (*p == ' ') {
        while (*p == ' ') p++;
        fields++;
      } else
        p++;
    }
    while (*p == ' ') p++;
    if (*p && *p != '\n') {
      r.path = p;
      while (!r.path.empty() &&
             (r.path.back() == '\n' || r.path.back() == '\r'))
        r.path.pop_back();
    }
    // 跳过过大的特殊区域
    if (r.end - r.start > 512ull * 1024 * 1024) continue;
    out.push_back(std::move(r));
  }
  std::fclose(f);
  return out;
}

static bool region_match_filter(const Region& r, RegionFilter f) {
  const std::string& p = r.path;
  auto has = [&](const char* s) { return p.find(s) != std::string::npos; };
  switch (f) {
    case RegionFilter::Writable:
      return r.writable && r.readable;
    case RegionFilter::Anonymous:
      return r.writable && (p.empty() || p[0] == '[' || has("anon"));
    case RegionFilter::Heap:
      return has("[heap]") || has("heap") || has("scudo") || has("jemalloc") ||
             has("malloc");
    case RegionFilter::Java:
      return has("dalvik") || has("art") || has("zygote") || has("jit-cache") ||
             has("dalvik-main space") || has("maple");
    case RegionFilter::CxxAlloc:
      return has("libc_malloc") || has("allocator") || has("GWP-ASan") ||
             has("malloc");
    case RegionFilter::Code:
      return r.readable && p.find(".so") != std::string::npos && !r.writable;
    case RegionFilter::Everything:
      return r.readable;
    default:
      return r.writable;
  }
}

std::vector<Region> load_maps_filtered(RegionFilter filter) {
  auto all = load_maps(false);
  std::vector<Region> out;
  out.reserve(all.size());
  for (auto& r : all) {
    if (region_match_filter(r, filter)) out.push_back(r);
  }
  return out;
}

const char* region_filter_name(RegionFilter f) {
  switch (f) {
    case RegionFilter::Writable:
      return "可写内存";
    case RegionFilter::Anonymous:
      return "匿名内存";
    case RegionFilter::Heap:
      return "堆 Heap";
    case RegionFilter::Java:
      return "Java/ART";
    case RegionFilter::CxxAlloc:
      return "C++分配器";
    case RegionFilter::Code:
      return "代码段.so";
    case RegionFilter::Everything:
      return "全部可读";
    default:
      return "?";
  }
}

bool parse_addr(const char* text, uintptr_t& out) {
  if (!text || !text[0]) return false;
  while (*text == ' ') text++;
  char* end = nullptr;
  unsigned long long v = std::strtoull(text, &end, 0);
  if (end == text) return false;
  out = (uintptr_t)v;
  return true;
}

void format_value(ValType type, uint64_t bits, char* out, size_t cap) {
  if (!out || cap < 2) return;
  switch (type) {
    case ValType::I8:
      std::snprintf(out, cap, "%d", (int)(int8_t)bits);
      break;
    case ValType::I16:
      std::snprintf(out, cap, "%d", (int)(int16_t)bits);
      break;
    case ValType::I32:
      std::snprintf(out, cap, "%d", (int)(int32_t)bits);
      break;
    case ValType::I64:
      std::snprintf(out, cap, "%lld", (long long)(int64_t)bits);
      break;
    case ValType::F32: {
      float f;
      uint32_t u = (uint32_t)bits;
      std::memcpy(&f, &u, 4);
      std::snprintf(out, cap, "%g", (double)f);
      break;
    }
    case ValType::F64: {
      double d;
      std::memcpy(&d, &bits, 8);
      std::snprintf(out, cap, "%g", d);
      break;
    }
    case ValType::Hex:
      std::snprintf(out, cap, "%02llX", (unsigned long long)(bits & 0xFF));
      break;
    case ValType::StrUtf8:
    case ValType::StrUtf16:
      // 无地址时仅显示占位；完整内容用 format_at
      std::snprintf(out, cap, "(str)");
      break;
    default:
      std::snprintf(out, cap, "%llu", (unsigned long long)bits);
      break;
  }
}

bool format_at(ValType type, uintptr_t addr, size_t match_len, char* out,
               size_t cap) {
  if (!out || cap < 2) return false;
  out[0] = 0;
  if (type == ValType::StrUtf8) {
    size_t n = match_len ? match_len : 32;
    if (n > 96) n = 96;
    std::vector<uint8_t> buf(n + 1, 0);
    if (!read_mem(addr, buf.data(), n)) {
      std::snprintf(out, cap, "(read fail)");
      return false;
    }
    // 可打印预览
    size_t w = 0;
    for (size_t i = 0; i < n && w + 1 < cap; ++i) {
      unsigned char c = buf[i];
      if (c == 0) break;
      if (c >= 32 && c < 127)
        out[w++] = (char)c;
      else if (c >= 0x80)
        out[w++] = (char)c;  // 保留 UTF-8 多字节
      else
        out[w++] = '.';
    }
    out[w] = 0;
    if (w == 0) std::snprintf(out, cap, "\"\"");
    return true;
  }
  if (type == ValType::StrUtf16) {
    size_t nbytes = match_len ? match_len : 64;
    if (nbytes > 128) nbytes = 128;
    nbytes &= ~1ull;
    std::vector<uint8_t> buf(nbytes + 2, 0);
    if (!read_mem(addr, buf.data(), nbytes)) {
      std::snprintf(out, cap, "(read fail)");
      return false;
    }
    // 简单 UTF-16LE → UTF-8（BMP）
    size_t w = 0;
    for (size_t i = 0; i + 1 < nbytes && w + 4 < cap; i += 2) {
      uint16_t u = (uint16_t)buf[i] | ((uint16_t)buf[i + 1] << 8);
      if (u == 0) break;
      if (u < 0x80)
        out[w++] = (char)u;
      else if (u < 0x800) {
        out[w++] = (char)(0xC0 | (u >> 6));
        out[w++] = (char)(0x80 | (u & 0x3F));
      } else {
        out[w++] = (char)(0xE0 | (u >> 12));
        out[w++] = (char)(0x80 | ((u >> 6) & 0x3F));
        out[w++] = (char)(0x80 | (u & 0x3F));
      }
    }
    out[w] = 0;
    if (w == 0) std::snprintf(out, cap, "\"\"");
    return true;
  }
  if (type == ValType::Hex) {
    size_t n = match_len ? match_len : 8;
    if (n > 24) n = 24;
    std::vector<uint8_t> buf(n);
    if (!read_mem(addr, buf.data(), n)) {
      std::snprintf(out, cap, "(read fail)");
      return false;
    }
    size_t w = 0;
    for (size_t i = 0; i < n && w + 3 < cap; ++i) {
      if (i) out[w++] = ' ';
      std::snprintf(out + w, cap - w, "%02X", buf[i]);
      w += 2;
    }
    return true;
  }
  uint64_t bits = 0;
  size_t sz = type_size_of(type);
  if (sz > 8) sz = 8;
  if (!read_mem(addr, &bits, sz)) {
    std::snprintf(out, cap, "(read fail)");
    return false;
  }
  format_value(type, bits, out, cap);
  return true;
}

/** UTF-8 文本 → UTF-16LE 字节 */
static bool utf8_to_utf16le_bytes(const char* s, std::vector<uint8_t>& out) {
  out.clear();
  if (!s || !s[0]) return false;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  while (*p) {
    uint32_t cp = 0;
    if (*p < 0x80) {
      cp = *p++;
    } else if ((*p & 0xE0) == 0xC0 && p[1]) {
      cp = (uint32_t)((*p & 0x1F) << 6) | (p[1] & 0x3F);
      p += 2;
    } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
      cp = (uint32_t)((*p & 0x0F) << 12) | (uint32_t)((p[1] & 0x3F) << 6) |
           (p[2] & 0x3F);
      p += 3;
    } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
      cp = (uint32_t)((*p & 0x07) << 18) | (uint32_t)((p[1] & 0x3F) << 12) |
           (uint32_t)((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
      p += 4;
    } else {
      return false;
    }
    if (cp < 0x10000) {
      out.push_back((uint8_t)(cp & 0xFF));
      out.push_back((uint8_t)((cp >> 8) & 0xFF));
    } else {
      cp -= 0x10000;
      uint16_t hi = (uint16_t)(0xD800 + (cp >> 10));
      uint16_t lo = (uint16_t)(0xDC00 + (cp & 0x3FF));
      out.push_back((uint8_t)(hi & 0xFF));
      out.push_back((uint8_t)((hi >> 8) & 0xFF));
      out.push_back((uint8_t)(lo & 0xFF));
      out.push_back((uint8_t)((lo >> 8) & 0xFF));
    }
  }
  return !out.empty();
}

bool parse_value(ValType type, const char* text, uint64_t& bits_out,
                 size_t& size_out) {
  if (!text) return false;
  while (*text == ' ') text++;
  size_out = type_size_of(type);
  bits_out = 0;
  if (type == ValType::F32) {
    char* end = nullptr;
    float f = std::strtof(text, &end);
    if (end == text) return false;
    uint32_t u;
    std::memcpy(&u, &f, 4);
    bits_out = u;
    return true;
  }
  if (type == ValType::F64) {
    char* end = nullptr;
    double d = std::strtod(text, &end);
    if (end == text) return false;
    std::memcpy(&bits_out, &d, 8);
    return true;
  }
  if (type == ValType::Hex) {
    // 单字节或整型 hex
    char* end = nullptr;
    unsigned long v = std::strtoul(text, &end, 16);
    if (end == text) return false;
    bits_out = v;
    return true;
  }
  char* end = nullptr;
  long long v = std::strtoll(text, &end, 0);
  if (end == text) return false;
  bits_out = (uint64_t)v;
  return true;
}

/** 解析 Hex 特征码，支持 ?? / ? 通配符。mask: 0=任意 0xFF=精确 */
static bool parse_hex_pattern(const char* text, std::vector<uint8_t>& out,
                              std::vector<uint8_t>& mask) {
  out.clear();
  mask.clear();
  if (!text) return false;
  const char* p = text;
  while (*p) {
    while (*p == ' ' || *p == ',' || *p == '-' || *p == ':' || *p == '\t') p++;
    if (!*p) break;
    // 通配：?? 或 ?
    if (*p == '?') {
      p++;
      if (*p == '?') p++;
      out.push_back(0);
      mask.push_back(0);
      continue;
    }
    // 半字节通配如 A? / ?B
    char c0 = p[0];
    char c1 = p[1] ? p[1] : 0;
    auto is_hex = [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
             (c >= 'A' && c <= 'F');
    };
    if (c0 == '?' && c1 && is_hex(c1)) {
      char tmp[3] = {c1, 0, 0};
      unsigned long v = std::strtoul(tmp, nullptr, 16);
      out.push_back((uint8_t)(v & 0xF));
      mask.push_back(0x0F);  // 仅低半字节
      p += 2;
      continue;
    }
    if (is_hex(c0) && c1 == '?') {
      char tmp[3] = {c0, 0, 0};
      unsigned long v = std::strtoul(tmp, nullptr, 16);
      out.push_back((uint8_t)((v & 0xF) << 4));
      mask.push_back(0xF0);
      p += 2;
      continue;
    }
    if (!is_hex(c0)) return false;
    char tmp[4] = {c0, (char)(is_hex(c1) ? c1 : 0), 0, 0};
    char* end = nullptr;
    unsigned long v = std::strtoul(tmp, &end, 16);
    if (end == tmp) return false;
    out.push_back((uint8_t)v);
    mask.push_back(0xFF);
    p += (tmp[1] ? 2 : 1);
  }
  // 掩码匹配时：pat 字节需 & mask
  for (size_t i = 0; i < out.size(); ++i) out[i] &= mask[i];
  return !out.empty();
}

bool parse_scan_values(ValType type, ScanMode mode, const char* v1,
                       const char* v2, ScanConfig& out, char* err,
                       size_t err_cap) {
  out = {};
  out.type = type;
  out.mode = mode;
  out.type_size = type_size_of(type);
  out.is_float = (type == ValType::F32 || type == ValType::F64);

  auto need_val = [&](ScanMode m) {
    return m == ScanMode::Exact || m == ScanMode::Greater ||
           m == ScanMode::Less || m == ScanMode::Between ||
           m == ScanMode::Fuzzy;
  };

  if (type == ValType::Hex) {
    if (mode == ScanMode::Unknown) {
      if (err) std::snprintf(err, err_cap, "Hex 不支持未知初始");
      return false;
    }
    if (!parse_hex_pattern(v1 ? v1 : "", out.hex_pat, out.hex_mask)) {
      if (err)
        std::snprintf(err, err_cap,
                      "Hex 格式错误，例: DE AD ?? EF 或 48 8B ??");
      return false;
    }
    out.type_size = out.hex_pat.size();
    return true;
  }

  if (type == ValType::StrUtf8 || type == ValType::StrUtf16) {
    if (mode == ScanMode::Unknown || mode == ScanMode::Changed ||
        mode == ScanMode::Unchanged || mode == ScanMode::Increased ||
        mode == ScanMode::Decreased) {
      if (err) std::snprintf(err, err_cap, "字符串仅支持精确/模糊搜索");
      return false;
    }
    if (!v1 || !v1[0]) {
      if (err) std::snprintf(err, err_cap, "请输入搜索字符串");
      return false;
    }
    // v2: "i" / "1" / "ci" → 忽略大小写
    out.str_case_insensitive = false;
    if (v2 && v2[0]) {
      if (v2[0] == 'i' || v2[0] == 'I' || v2[0] == '1' ||
          std::strcmp(v2, "ci") == 0 || std::strcmp(v2, "ignore") == 0)
        out.str_case_insensitive = true;
    }
    // 模糊模式：字符串也按精确字节匹配（容差不适用），仅允许 Exact/Fuzzy
    if (mode == ScanMode::Greater || mode == ScanMode::Less ||
        mode == ScanMode::Between) {
      if (err) std::snprintf(err, err_cap, "字符串不支持大于/小于/区间");
      return false;
    }
    if (type == ValType::StrUtf8) {
      size_t len = std::strlen(v1);
      if (len > 256) len = 256;
      out.hex_pat.assign(reinterpret_cast<const uint8_t*>(v1),
                         reinterpret_cast<const uint8_t*>(v1) + len);
    } else {
      if (!utf8_to_utf16le_bytes(v1, out.hex_pat)) {
        if (err) std::snprintf(err, err_cap, "UTF-16 编码失败");
        return false;
      }
    }
    out.hex_mask.assign(out.hex_pat.size(), 0xFF);
    out.type_size = out.hex_pat.size();
    return true;
  }

  if (mode == ScanMode::Unknown || mode == ScanMode::Changed ||
      mode == ScanMode::Unchanged || mode == ScanMode::Increased ||
      mode == ScanMode::Decreased) {
    // 可不填数值
    if (need_val(mode) && (!v1 || !v1[0])) {
      // not
    }
    if (!need_val(mode)) return true;
  }

  if (need_val(mode)) {
    if (!v1 || !v1[0]) {
      if (err) std::snprintf(err, err_cap, "请输入搜索数值");
      return false;
    }
    if (out.is_float) {
      char* end = nullptr;
      out.a_f = std::strtod(v1, &end);
      if (end == v1) {
        if (err) std::snprintf(err, err_cap, "浮点解析失败");
        return false;
      }
      if (type == ValType::F32) {
        float f = (float)out.a_f;
        uint32_t u;
        std::memcpy(&u, &f, 4);
        out.a_bits = u;
      } else {
        std::memcpy(&out.a_bits, &out.a_f, 8);
      }
    } else {
      size_t sz = 0;
      if (!parse_value(type, v1, out.a_bits, sz)) {
        if (err) std::snprintf(err, err_cap, "数值解析失败");
        return false;
      }
      out.a_f = bits_to_f(type, out.a_bits);
    }
  }

  if (mode == ScanMode::Between) {
    if (!v2 || !v2[0]) {
      if (err) std::snprintf(err, err_cap, "请输入区间上限");
      return false;
    }
    if (out.is_float) {
      char* end = nullptr;
      out.b_f = std::strtod(v2, &end);
      if (end == v2) {
        if (err) std::snprintf(err, err_cap, "上限解析失败");
        return false;
      }
    } else {
      size_t sz = 0;
      if (!parse_value(type, v2, out.b_bits, sz)) {
        if (err) std::snprintf(err, err_cap, "上限解析失败");
        return false;
      }
      out.b_f = bits_to_f(type, out.b_bits);
    }
  }
  if (mode == ScanMode::Fuzzy) {
    // v2 = 容差，默认 1
    out.fuzzy_tol = 1.0;
    if (v2 && v2[0]) {
      char* end = nullptr;
      double t = std::strtod(v2, &end);
      if (end != v2 && t >= 0) out.fuzzy_tol = t;
    }
  }
  return true;
}

bool fuzzy_match(const char* text, const char* pattern) {
  if (!pattern || !pattern[0]) return true;
  if (!text) return false;
  // 多 token（空格分隔）均需命中
  auto fold = [](char c) -> char {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
  };
  auto subseq = [&](const char* hay, const char* needle) -> bool {
    if (!needle[0]) return true;
    const char* h = hay;
    const char* n = needle;
    while (*h && *n) {
      if (fold(*h) == fold(*n)) n++;
      h++;
    }
    return *n == 0;
  };
  auto substr = [&](const char* hay, const char* needle) -> bool {
    size_t nl = std::strlen(needle);
    if (nl == 0) return true;
    size_t hl = std::strlen(hay);
    if (nl > hl) return false;
    for (size_t i = 0; i + nl <= hl; ++i) {
      bool ok = true;
      for (size_t j = 0; j < nl; ++j) {
        if (fold(hay[i + j]) != fold(needle[j])) {
          ok = false;
          break;
        }
      }
      if (ok) return true;
    }
    return false;
  };

  // 复制 pattern 按空格拆
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%s", pattern);
  char* save = nullptr;
  char* tok = strtok_r(buf, " \t", &save);
  while (tok) {
    bool hit = substr(text, tok) || subseq(text, tok);
    // 包名按 . 分段再匹配（qq 命中 mobileqq；tenc 命中 tencent）
    if (!hit) {
      const char* s = text;
      while (*s && !hit) {
        const char* e = s;
        while (*e && *e != '.' && *e != ':' && *e != '/') e++;
        char seg[96];
        size_t n = (size_t)(e - s);
        if (n >= sizeof(seg)) n = sizeof(seg) - 1;
        std::memcpy(seg, s, n);
        seg[n] = 0;
        if (substr(seg, tok) || subseq(seg, tok)) hit = true;
        s = *e ? e + 1 : e;
      }
    }
    if (!hit) return false;
    tok = strtok_r(nullptr, " \t", &save);
  }
  return true;
}

bool start_first_scan(const ScanConfig& cfg) {
  if (!is_attached()) {
    set_status("请先附加进程");
    return false;
  }
  if (g_busy.load()) return false;
  join_worker();
  g_cancel = false;
  g_busy = true;
  g_progress = 0.f;
  set_status("首次搜索中…");
  g_last_type = cfg.type;

  ScanConfig cfg_copy = cfg;
  g_worker = std::thread([cfg_copy]() {
    std::vector<Match> found;
    found.reserve(4096);
    scan_regions_first(cfg_copy, found);
    {
      std::lock_guard<std::mutex> lk(g_mu);
      g_results.swap(found);
      g_round = 1;
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "首次搜索完成 · %zu 条",
                  g_results.size());
    set_status(buf);
    g_busy = false;
  });
  return true;
}

bool start_next_scan(const ScanConfig& cfg) {
  if (!is_attached()) {
    set_status("请先附加进程");
    return false;
  }
  if (g_busy.load()) return false;
  if (g_round <= 0) {
    set_status("请先首次搜索");
    return false;
  }
  join_worker();
  g_cancel = false;
  g_busy = true;
  g_progress = 0.f;
  set_status("再次筛选中…");
  g_last_type = cfg.type;

  ScanConfig cfg_copy = cfg;
  g_worker = std::thread([cfg_copy]() {
    std::vector<Match> cur;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      cur = g_results;
    }
    scan_filter_next(cfg_copy, cur);
    {
      std::lock_guard<std::mutex> lk(g_mu);
      g_results.swap(cur);
      g_round++;
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "筛选完成 · %zu 条", g_results.size());
    set_status(buf);
    g_busy = false;
  });
  return true;
}

void clear_scan() {
  if (g_busy.load()) {
    g_cancel = true;
    join_worker();
    g_busy = false;
  }
  std::lock_guard<std::mutex> lk(g_mu);
  g_results.clear();
  g_round = 0;
  set_status("已清空搜索");
}

bool scan_busy() { return g_busy.load(); }
float scan_progress() { return g_progress.load(); }
const char* scan_status() { return g_status; }
int scan_round() { return g_round; }

size_t result_count() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_results.size();
}

ValType last_scan_type() { return g_last_type; }

void copy_results_range(std::vector<Match>& out, size_t offset, size_t count) {
  std::lock_guard<std::mutex> lk(g_mu);
  out.clear();
  if (offset >= g_results.size() || count == 0) return;
  size_t n = std::min(count, g_results.size() - offset);
  out.assign(g_results.begin() + (std::ptrdiff_t)offset,
             g_results.begin() + (std::ptrdiff_t)(offset + n));
}

void copy_results(std::vector<Match>& out, size_t max_n) {
  copy_results_range(out, 0, max_n);
}

void refresh_result_values(std::vector<Match>& io) {
  if (is_pattern_type(g_last_type)) {
    for (auto& m : io) {
      size_t n = m.match_len ? m.match_len : 8;
      if (n > 8) n = 8;
      uint8_t tmp[8]{};
      if (read_mem(m.addr, tmp, n)) {
        m.value_bits = 0;
        std::memcpy(&m.value_bits, tmp, n);
      }
    }
    return;
  }
  size_t sz = type_size_of(g_last_type);
  if (sz > 8) sz = 8;
  for (auto& m : io) {
    uint8_t tmp[8]{};
    if (read_mem(m.addr, tmp, sz)) m.value_bits = load_bits(tmp, sz);
  }
}

void set_frozen(uintptr_t addr, bool on, uint64_t bits, size_t type_size) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = std::find_if(g_freeze.begin(), g_freeze.end(),
                         [&](const FreezeItem& f) { return f.addr == addr; });
  if (on) {
    if (it != g_freeze.end()) {
      it->bits = bits;
      it->size = type_size;
    } else {
      g_freeze.push_back({addr, bits, type_size});
    }
    // 同步 results 标记
    for (auto& m : g_results) {
      if (m.addr == addr) {
        m.frozen = true;
        m.freeze_bits = bits;
      }
    }
  } else {
    if (it != g_freeze.end()) g_freeze.erase(it);
    for (auto& m : g_results) {
      if (m.addr == addr) m.frozen = false;
    }
  }
}

void clear_all_frozen() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_freeze.clear();
  for (auto& m : g_results) m.frozen = false;
}

void tick_freeze(ValType type) {
  size_t def_sz = type_size_of(type);
  std::vector<FreezeItem> copy;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    copy = g_freeze;
  }
  for (auto& f : copy) {
    size_t sz = f.size ? f.size : def_sz;
    if (sz > 8) sz = 8;
    write_mem(f.addr, &f.bits, sz);
  }
}

int export_results(const char* path) {
  if (!path || !path[0]) return -1;
  std::lock_guard<std::mutex> lk(g_mu);
  FILE* f = std::fopen(path, "w");
  if (!f) return -1;
  int n = 0;
  for (auto& m : g_results) {
    std::fprintf(f, "0x%llX\n", (unsigned long long)m.addr);
    n++;
  }
  std::fclose(f);
  return n;
}

int dump_mem(uintptr_t addr, size_t len, const char* path) {
  if (!path || !is_attached() || len == 0 || len > 64 * 1024 * 1024) return -1;
  FILE* f = std::fopen(path, "wb");
  if (!f) return -1;
  std::vector<uint8_t> buf(std::min(len, kChunk));
  size_t left = len;
  uintptr_t cur = addr;
  size_t written = 0;
  while (left > 0) {
    size_t n = std::min(left, buf.size());
    if (!read_mem(cur, buf.data(), n)) break;
    std::fwrite(buf.data(), 1, n, f);
    written += n;
    cur += n;
    left -= n;
  }
  std::fclose(f);
  return (int)written;
}

}  // namespace mem
