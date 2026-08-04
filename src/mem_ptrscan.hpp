#pragma once
/**
 * CE 风格指针扫描（多级）
 * 从目标地址向上找「静态基址 + 偏移链」
 */
#include "mem_core.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace mem {

struct PtrChain {
  uintptr_t base = 0;           // 第一级指针槽绝对地址
  uintptr_t module_base = 0;    // 所属模块映射起点（0=未知）
  uintptr_t base_rva = 0;       // base - module_base
  std::string module;           // .so 文件名
  std::string module_path;      // 完整 path（可选）
  std::vector<int32_t> offsets; // base -> ... -> target
  uintptr_t resolved = 0;       // 上次解析终点
  bool valid = false;           // 最近一次 resolve 是否成功
};

struct PtrScanConfig {
  uintptr_t target = 0;
  int max_level = 3;       // 1..5
  uint32_t max_offset = 0x1000;
  bool static_only = true; // 基址仅限文件映射（.so 等）
  int max_results = 200;
};

bool ptrscan_start(const PtrScanConfig& cfg);
bool ptrscan_busy();
float ptrscan_progress();
const char* ptrscan_status();
void ptrscan_clear();
size_t ptrscan_count();
void ptrscan_copy(std::vector<PtrChain>& out, size_t max_n = 100);

/** 按链解析当前值，成功返回最终地址 */
bool ptrscan_resolve(const PtrChain& chain, uintptr_t& out_addr);
/** 解析并返回每一步地址：slots[0]=base, 然后每级指针槽/终点 */
bool ptrscan_resolve_path(const PtrChain& chain, std::vector<uintptr_t>& path);

/** 格式化模板: libfoo.so+0x1234 -> +0x10 -> +0x20 */
void ptrscan_format(const PtrChain& chain, char* buf, size_t cap);
/** 从模板串解析（简化: module+rva,off0,off1,...） */
bool ptrscan_parse_template(const char* text, PtrChain& out);

/** 按模块名重定位 base（重启 App 后用） */
bool ptrscan_rebind(PtrChain& chain);
/** 批量验证：resolve 后写 valid/resolved，返回仍有效条数 */
int ptrscan_verify_all(std::vector<PtrChain>& chains);

/** 存盘 / 加载指针链模板 */
int ptrscan_save(const char* path, const std::vector<PtrChain>& chains);
int ptrscan_load(const char* path, std::vector<PtrChain>& chains);

/** 查找模块基址（文件名匹配） */
bool find_module_base(const char* module_name, uintptr_t& out_base,
                      uintptr_t& out_end, std::string* full_path = nullptr);

}  // namespace mem
