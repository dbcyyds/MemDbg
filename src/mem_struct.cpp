#include "mem_struct.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mem {

const char* field_type_name(FieldType t) {
  switch (t) {
    case FieldType::I8:
      return "I8";
    case FieldType::I16:
      return "I16";
    case FieldType::I32:
      return "I32";
    case FieldType::I64:
      return "I64";
    case FieldType::U32:
      return "U32";
    case FieldType::U64:
      return "U64";
    case FieldType::F32:
      return "F32";
    case FieldType::F64:
      return "F64";
    case FieldType::Ptr:
      return "Ptr";
    case FieldType::Hex4:
      return "Hex4";
    case FieldType::Hex8:
      return "Hex8";
    case FieldType::Utf8:
      return "UTF8";
    case FieldType::Utf16:
      return "U16LE";
    case FieldType::Bytes:
      return "Bytes";
    case FieldType::Padding:
      return "Pad";
    default:
      return "?";
  }
}

size_t field_type_size(FieldType t, int user_size) {
  switch (t) {
    case FieldType::I8:
      return 1;
    case FieldType::I16:
      return 2;
    case FieldType::I32:
    case FieldType::U32:
    case FieldType::F32:
    case FieldType::Hex4:
      return 4;
    case FieldType::I64:
    case FieldType::U64:
    case FieldType::F64:
    case FieldType::Ptr:
    case FieldType::Hex8:
      return 8;
    case FieldType::Utf8:
    case FieldType::Utf16:
    case FieldType::Bytes:
    case FieldType::Padding:
      return user_size > 0 ? (size_t)user_size : 16;
    default:
      return 4;
  }
}

void Structure::clear() {
  fields.clear();
  base = 0;
  std::snprintf(name, sizeof(name), "Struct");
}

void Structure::add_field(const char* n, FieldType type, int32_t offset,
                          int size) {
  StructField f;
  std::snprintf(f.name, sizeof(f.name), "%s", n ? n : "field");
  f.type = type;
  f.offset = offset;
  f.size = size;
  fields.push_back(f);
}

void Structure::append_field(const char* n, FieldType type, int size) {
  int off = total_size();
  // 指针/8 字节对齐
  size_t sz = field_type_size(type, size);
  if (sz >= 8) off = (off + 7) & ~7;
  else if (sz >= 4) off = (off + 3) & ~3;
  add_field(n, type, off, size);
}

void Structure::remove_selected() {
  fields.erase(std::remove_if(fields.begin(), fields.end(),
                              [](const StructField& f) { return f.selected; }),
               fields.end());
}

int Structure::total_size() const {
  int max_end = 0;
  for (auto& f : fields) {
    int end = f.offset + (int)field_type_size(f.type, f.size);
    if (end > max_end) max_end = end;
  }
  return max_end;
}

void Structure::refresh() {
  if (!is_attached() || base == 0) {
    for (auto& f : fields) {
      std::snprintf(f.value, sizeof(f.value), "—");
      f.as_ptr = 0;
    }
    return;
  }
  for (auto& f : fields) {
    f.as_ptr = 0;
    uintptr_t addr = base + (uintptr_t)(int64_t)f.offset;
    size_t sz = field_type_size(f.type, f.size);
    if (f.type == FieldType::Padding) {
      std::snprintf(f.value, sizeof(f.value), "<%d bytes>", (int)sz);
      continue;
    }
    if (f.type == FieldType::Utf8 || f.type == FieldType::Utf16 ||
        f.type == FieldType::Bytes) {
      format_at(f.type == FieldType::Utf16 ? ValType::StrUtf16
                : f.type == FieldType::Utf8 ? ValType::StrUtf8
                                            : ValType::Hex,
                addr, sz, f.value, sizeof(f.value));
      continue;
    }
    if (f.type == FieldType::Ptr) {
      uint64_t p = 0;
      if (read_mem(addr, &p, 8)) {
        f.as_ptr = (uintptr_t)p;
        std::snprintf(f.value, sizeof(f.value), "0x%llX",
                      (unsigned long long)p);
      } else {
        std::snprintf(f.value, sizeof(f.value), "(read fail)");
      }
      continue;
    }
    if (f.type == FieldType::Hex4 || f.type == FieldType::Hex8) {
      format_at(ValType::Hex, addr, sz, f.value, sizeof(f.value));
      continue;
    }
    ValType vt = ValType::I32;
    switch (f.type) {
      case FieldType::I8:
        vt = ValType::I8;
        break;
      case FieldType::I16:
        vt = ValType::I16;
        break;
      case FieldType::I32:
      case FieldType::U32:
        vt = ValType::I32;
        break;
      case FieldType::I64:
      case FieldType::U64:
        vt = ValType::I64;
        break;
      case FieldType::F32:
        vt = ValType::F32;
        break;
      case FieldType::F64:
        vt = ValType::F64;
        break;
      default:
        break;
    }
    format_at(vt, addr, sz, f.value, sizeof(f.value));
    if (f.type == FieldType::U32 || f.type == FieldType::U64) {
      uint64_t bits = 0;
      if (read_mem(addr, &bits, sz)) {
        if (f.type == FieldType::U32)
          std::snprintf(f.value, sizeof(f.value), "%u", (unsigned)(uint32_t)bits);
        else
          std::snprintf(f.value, sizeof(f.value), "%llu",
                        (unsigned long long)bits);
      }
    }
  }
}

void Structure::auto_dissect(int max_fields) {
  fields.clear();
  if (max_fields < 4) max_fields = 4;
  if (max_fields > 64) max_fields = 64;
  if (!is_attached() || base == 0) {
    // 默认模板
    for (int i = 0; i < max_fields; ++i) {
      char nm[32];
      std::snprintf(nm, sizeof(nm), "f%d", i);
      if (i % 4 == 0)
        append_field(nm, FieldType::Ptr);
      else if (i % 4 == 1)
        append_field(nm, FieldType::I32);
      else if (i % 4 == 2)
        append_field(nm, FieldType::I32);
      else
        append_field(nm, FieldType::F32);
    }
    return;
  }
  // 读 256 字节，启发式：像指针则 Ptr，否则 I32
  uint8_t buf[256]{};
  size_t got = 256;
  if (!read_mem(base, buf, got)) got = 0;
  int off = 0;
  int n = 0;
  while (n < max_fields && off + 4 <= (int)got) {
    char nm[32];
    std::snprintf(nm, sizeof(nm), "+0x%X", off);
    if (off + 8 <= (int)got && (off % 8) == 0) {
      uint64_t p = 0;
      std::memcpy(&p, buf + off, 8);
      uint32_t hi = (uint32_t)(p >> 32);
      uint32_t lo = (uint32_t)p;
      // 启发式：排除「两个小整数拼在一起」；允许低堆/高地址用户指针
      bool looks_ptr = false;
      if (p > 0x10000ull && p < 0x0000800000000000ull) {
        if (hi == 0)
          looks_ptr = lo > 0x10000u;  // 32 位形态地址
        else if (hi >= 0x100)
          looks_ptr = true;  // 典型 64 位用户指针
        // else hi 很小 → 多半是两个 I32
      }
      if (looks_ptr) {
        add_field(nm, FieldType::Ptr, off);
        off += 8;
        n++;
        continue;
      }
    }
    add_field(nm, FieldType::I32, off);
    off += 4;
    n++;
  }
  while (n < max_fields) {
    char nm[32];
    std::snprintf(nm, sizeof(nm), "+0x%X", off);
    add_field(nm, FieldType::I32, off);
    off += 4;
    n++;
  }
  refresh();
}

int Structure::save(const char* path) const {
  if (!path) return -1;
  FILE* f = std::fopen(path, "w");
  if (!f) return -1;
  std::fprintf(f, "MEMDBG_STRUCT 1\n");
  std::fprintf(f, "name %s\n", name);
  std::fprintf(f, "base 0x%llX\n", (unsigned long long)base);
  std::fprintf(f, "fields %zu\n", fields.size());
  for (auto& fd : fields) {
    std::fprintf(f, "f %d %d %d %s\n", (int)fd.type, fd.offset, fd.size,
                 fd.name);
  }
  std::fclose(f);
  return (int)fields.size();
}

int Structure::load(const char* path) {
  if (!path) return -1;
  FILE* f = std::fopen(path, "r");
  if (!f) return -1;
  char line[256];
  if (!std::fgets(line, sizeof(line), f) ||
      std::strncmp(line, "MEMDBG_STRUCT", 13) != 0) {
    std::fclose(f);
    return -1;
  }
  fields.clear();
  while (std::fgets(line, sizeof(line), f)) {
    if (std::strncmp(line, "name ", 5) == 0) {
      std::snprintf(name, sizeof(name), "%s", line + 5);
      // trim newline
      size_t n = std::strlen(name);
      while (n && (name[n - 1] == '\n' || name[n - 1] == '\r')) name[--n] = 0;
    } else if (std::strncmp(line, "base ", 5) == 0) {
      unsigned long long b = 0;
      std::sscanf(line + 5, "%llx", &b);
      base = (uintptr_t)b;
    } else if (line[0] == 'f' && line[1] == ' ') {
      int ty = 0, off = 0, sz = 0;
      char nm[48]{};
      if (std::sscanf(line + 2, "%d %d %d %47s", &ty, &off, &sz, nm) >= 3) {
        StructField fd;
        fd.type = (FieldType)ty;
        fd.offset = off;
        fd.size = sz;
        std::snprintf(fd.name, sizeof(fd.name), "%s", nm[0] ? nm : "field");
        fields.push_back(fd);
      }
    }
  }
  std::fclose(f);
  refresh();
  return (int)fields.size();
}

bool struct_follow_ptr(const Structure& s, int field_index, uintptr_t& out_addr) {
  if (field_index < 0 || field_index >= (int)s.fields.size()) return false;
  auto& f = s.fields[(size_t)field_index];
  if (f.type != FieldType::Ptr || f.as_ptr == 0) return false;
  out_addr = f.as_ptr;
  return true;
}

}  // namespace mem
