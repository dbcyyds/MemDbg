#pragma once
/**
 * CE 风格结构体解析（Structure Dissect）
 * - 在基址上定义字段偏移/类型
 * - 实时读值、跟随指针、存盘加载
 */
#include "mem_core.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace mem {

enum class FieldType : int {
  I8 = 0,
  I16,
  I32,
  I64,
  U32,
  U64,
  F32,
  F64,
  Ptr,      // 8 字节指针（ARM64）
  Hex4,     // 4 字节 hex
  Hex8,
  Utf8,     // 以 0 结尾，最多 size 字节预览
  Utf16,    // UTF-16LE
  Bytes,    // 原始字节预览 size
  Padding,  // 仅占位
  COUNT
};

const char* field_type_name(FieldType t);
size_t field_type_size(FieldType t, int user_size = 0);

struct StructField {
  char name[48] = "field";
  FieldType type = FieldType::I32;
  int32_t offset = 0;   // 相对结构基址
  int size = 0;         // Utf8/Bytes 等可变长度；0=用类型默认
  char value[96] = "";  // 显示缓存
  uintptr_t as_ptr = 0; // Ptr 类型解析出的地址
  bool selected = false;
};

struct Structure {
  char name[64] = "Struct";
  uintptr_t base = 0;
  std::vector<StructField> fields;

  void clear();
  void add_field(const char* name, FieldType type, int32_t offset,
                 int size = 0);
  /** 在末尾追加字段（自动推 offset） */
  void append_field(const char* name, FieldType type, int size = 0);
  void remove_selected();
  /** 从内存刷新所有字段 value */
  void refresh();
  /** 当前结构占用估算（最大 offset+size） */
  int total_size() const;

  int save(const char* path) const;
  int load(const char* path);

  /** 在 base 处自动猜测若干字段（前 n 个 I32/Ptr 交替探测） */
  void auto_dissect(int max_fields = 16);
};

/** 跟随指针字段：新结构基址 = field.as_ptr */
bool struct_follow_ptr(const Structure& s, int field_index, uintptr_t& out_addr);

}  // namespace mem
