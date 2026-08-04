#pragma once
/**
 * CE 风格地址表 / 作弊表
 * 独立于扫描结果，可冻结、改类型、存盘加载
 */
#include "mem_core.hpp"
#include <string>
#include <vector>

namespace mem {

struct TableEntry {
  uintptr_t addr = 0;
  ValType type = ValType::I32;
  bool active = true;   // 是否启用
  bool freeze = false;
  uint64_t freeze_bits = 0;
  char desc[64] = "地址";
  char value[48] = "";  // 显示缓存
  bool selected = false;
};

class AddressTable {
 public:
  std::vector<TableEntry> entries;

  void add(uintptr_t addr, ValType type, const char* desc = nullptr);
  void remove_selected();
  void clear();
  void refresh_values();  // 读内存刷新 value 显示
  void tick_freeze();     // 写回冻结

  int save(const char* path) const;
  int load(const char* path);

  size_t size() const { return entries.size(); }
};

}  // namespace mem
