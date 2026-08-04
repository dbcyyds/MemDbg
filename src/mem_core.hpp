#pragma once
/**
 * MemDbg 内存引擎（root）
 * - 进程列表 / 附加
 * - process_vm_readv/writev + /proc/pid/mem 回退
 * - 数值搜索 / 再次筛选
 * - 冻结写回
 */
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mem {

enum class ValType : int {
  I8 = 0,
  I16,
  I32,
  I64,
  F32,
  F64,
  Hex,      // 十六进制字节串 / AoB（支持 ??）
  StrUtf8,  // UTF-8 字符串
  StrUtf16, // UTF-16LE 字符串
  COUNT
};

enum class ScanMode : int {
  Exact = 0,
  Greater,
  Less,
  Between,
  Changed,
  Unchanged,
  Increased,
  Decreased,
  Unknown,  // 未知初始：记录全部候选地址
  Fuzzy,    // 模糊：|value-target| <= tol（整数 tol 或浮点容差）
  COUNT
};

struct ProcInfo {
  int pid = 0;
  std::string name;       // 显示名 / 包名
  std::string package;    // 包名（若可解析）
  bool has_icon = false;  // 有应用图标（可解析到 APK/数据目录）
  bool is_tencent = false;
  int icon_color = 0;     // 图标底色哈希
};

struct ProcListOptions {
  const char* filter = nullptr;  // 支持模糊匹配（子序列/忽略大小写）
  bool skip_system = false;      // 默认不过滤系统（UI 已移除该开关）
  bool skip_no_icon = true;      // 过滤没有应用图标的进程
  bool tencent_only = false;     // 仅腾讯系
  bool fuzzy_filter = true;      // 进程名模糊过滤
};

struct Region {
  uintptr_t start = 0;
  uintptr_t end = 0;
  char perms[5]{};  // "rw-p"
  std::string path;
  bool readable = false;
  bool writable = false;
};

struct Match {
  uintptr_t addr = 0;
  uint64_t value_bits = 0;  // 当前值原始位（按类型宽度）
  uint64_t prev_bits = 0;   // 上一轮值
  bool frozen = false;
  uint64_t freeze_bits = 0;  // 冻结写入值
  size_t match_len = 0;      // 字符串/Hex 匹配长度（字节）
};

/** 扫描内存区域过滤（CE 风格） */
enum class RegionFilter : int {
  Writable = 0,  // 全部可写
  Anonymous,     // 匿名 [anon] / 无 path
  Heap,          // [heap] / allocator
  Java,          // dalvik / art / scudo 等
  CxxAlloc,      // libc_malloc / allocator
  Code,          // r-x 可执行（只读代码，搜常量）
  Everything,    // 所有可读
  COUNT
};

struct ScanConfig {
  ValType type = ValType::I32;
  ScanMode mode = ScanMode::Exact;
  RegionFilter region = RegionFilter::Writable;
  // 解析后的比较值（bits 或数值）
  uint64_t a_bits = 0;
  uint64_t b_bits = 0;  // Between 上限
  double a_f = 0.0;
  double b_f = 0.0;
  bool is_float = false;
  size_t type_size = 4;
  // Hex / 字符串模式：字节序列；hex_mask[i]==0 表示通配（??）
  std::vector<uint8_t> hex_pat;
  std::vector<uint8_t> hex_mask;  // 0=任意, 0xFF=精确（字符串全 0xFF）
  // Fuzzy 模式容差（整数绝对值 / 浮点绝对容差）
  double fuzzy_tol = 1.0;
  // 字符串：忽略大小写（仅 ASCII A-Z）
  bool str_case_insensitive = false;
};

// ── 进程 ──────────────────────────────────────────────────
std::vector<ProcInfo> list_processes(const ProcListOptions& opt = {});
/** 兼容旧调用 */
std::vector<ProcInfo> list_processes(const char* filter, bool skip_system);
bool is_system_process(int pid, const std::string& name);
bool is_tencent_package(const std::string& name);
bool process_has_app_icon(int pid, const std::string& name);
bool attach(int pid);
void detach();
bool is_attached();
int attached_pid();
const char* attached_name();

// ── 读写 ──────────────────────────────────────────────────
bool read_mem(uintptr_t addr, void* buf, size_t len);
bool write_mem(uintptr_t addr, const void* buf, size_t len);

// ── 映射 ──────────────────────────────────────────────────
std::vector<Region> load_maps(bool writable_only = true);
std::vector<Region> load_maps_filtered(RegionFilter filter);
const char* region_filter_name(RegionFilter f);

// ── 搜索 ──────────────────────────────────────────────────
/** 解析 UI 输入到 ScanConfig，失败返回 false */
bool parse_scan_values(ValType type, ScanMode mode, const char* v1,
                       const char* v2, ScanConfig& out, char* err, size_t err_cap);

/** 异步：首次搜索 / 再次筛选。busy() 为 true 时进行中 */
bool start_first_scan(const ScanConfig& cfg);
bool start_next_scan(const ScanConfig& cfg);
void clear_scan();
bool scan_busy();
float scan_progress();  // 0..1
const char* scan_status();
int scan_round();
size_t result_count();
/** 上次扫描类型（供 UI 格式化） */
ValType last_scan_type();
/** UI 显示用：从 offset 起最多 count 条（分页） */
void copy_results_range(std::vector<Match>& out, size_t offset, size_t count);
/** 兼容：从 0 起最多 max_n 条 */
void copy_results(std::vector<Match>& out, size_t max_n = 300);
/** 刷新结果列表中的实时值（已显示的地址） */
void refresh_result_values(std::vector<Match>& io);
/** 从内存格式化显示值（支持字符串） */
bool format_at(ValType type, uintptr_t addr, size_t match_len, char* out,
               size_t cap);
/** 是否为按字节模式匹配的类型（Hex/字符串） */
bool is_pattern_type(ValType t);

// ── 冻结 ──────────────────────────────────────────────────
void set_frozen(uintptr_t addr, bool on, uint64_t bits, size_t type_size);
void clear_all_frozen();
/** 每帧调用：写回所有冻结地址 */
void tick_freeze(ValType type);

// ── 工具 ──────────────────────────────────────────────────
/** 导出当前结果地址表到 path，返回写入条数，失败 -1 */
int export_results(const char* path);
/** Dump 指定地址长度到文件，返回写入字节，失败 -1 */
int dump_mem(uintptr_t addr, size_t len, const char* path);

/** 将 value_bits 格式化为字符串 */
void format_value(ValType type, uint64_t bits, char* out, size_t cap);
/** 解析写入字符串为 bits */
bool parse_value(ValType type, const char* text, uint64_t& bits_out,
                 size_t& size_out);
/** 解析地址字符串 0x... */
bool parse_addr(const char* text, uintptr_t& out);

size_t type_size_of(ValType t);

/** 模糊字符串匹配：子串 / 子序列 / 忽略大小写 */
bool fuzzy_match(const char* text, const char* pattern);

}  // namespace mem
