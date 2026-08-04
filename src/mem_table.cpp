#include "mem_table.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mem {

void AddressTable::add(uintptr_t addr, ValType type, const char* desc) {
  for (auto& e : entries) {
    if (e.addr == addr && e.type == type) {
      if (desc && desc[0])
        std::snprintf(e.desc, sizeof(e.desc), "%s", desc);
      return;
    }
  }
  TableEntry e;
  e.addr = addr;
  e.type = type;
  if (desc && desc[0])
    std::snprintf(e.desc, sizeof(e.desc), "%s", desc);
  else
    std::snprintf(e.desc, sizeof(e.desc), "地址");
  uint64_t bits = 0;
  size_t sz = type_size_of(type);
  if (sz > 8) sz = 8;
  if (is_attached() && read_mem(addr, &bits, sz)) {
    e.freeze_bits = bits;
    format_value(type, bits, e.value, sizeof(e.value));
  }
  entries.push_back(e);
}

void AddressTable::remove_selected() {
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [](const TableEntry& e) { return e.selected; }),
                entries.end());
}

void AddressTable::clear() { entries.clear(); }

void AddressTable::refresh_values() {
  if (!is_attached()) return;
  for (auto& e : entries) {
    if (!e.active) continue;
    uint64_t bits = 0;
    size_t sz = type_size_of(e.type);
    if (sz > 8) sz = 8;
    if (e.type == ValType::Hex) sz = 1;
    if (read_mem(e.addr, &bits, sz)) {
      if (!e.freeze) e.freeze_bits = bits;
      format_value(e.type, bits, e.value, sizeof(e.value));
    } else {
      std::snprintf(e.value, sizeof(e.value), "??");
    }
  }
}

void AddressTable::tick_freeze() {
  if (!is_attached()) return;
  for (auto& e : entries) {
    if (!e.active || !e.freeze) continue;
    size_t sz = type_size_of(e.type);
    if (sz > 8) sz = 8;
    write_mem(e.addr, &e.freeze_bits, sz);
  }
}

int AddressTable::save(const char* path) const {
  if (!path) return -1;
  FILE* f = std::fopen(path, "w");
  if (!f) return -1;
  std::fprintf(f, "# MemDbg CheatTable v1\n");
  std::fprintf(f, "# addr type freeze bits description\n");
  int n = 0;
  for (auto& e : entries) {
    std::fprintf(f, "0x%llX %d %d 0x%llX %s\n",
                 (unsigned long long)e.addr, (int)e.type, e.freeze ? 1 : 0,
                 (unsigned long long)e.freeze_bits, e.desc);
    n++;
  }
  std::fclose(f);
  return n;
}

int AddressTable::load(const char* path) {
  if (!path) return -1;
  FILE* f = std::fopen(path, "r");
  if (!f) return -1;
  entries.clear();
  char line[256];
  int n = 0;
  while (std::fgets(line, sizeof(line), f)) {
    if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
    unsigned long long addr = 0, bits = 0;
    int type = 0, fr = 0;
    char desc[64]{};
    if (std::sscanf(line, "0x%llx %d %d 0x%llx %63[^\n]", &addr, &type, &fr,
                    &bits, desc) >= 4) {
      TableEntry e;
      e.addr = (uintptr_t)addr;
      e.type = (ValType)type;
      if (e.type < ValType::I8 || e.type >= ValType::COUNT)
        e.type = ValType::I32;
      e.freeze = fr != 0;
      e.freeze_bits = bits;
      if (desc[0])
        std::snprintf(e.desc, sizeof(e.desc), "%s", desc);
      format_value(e.type, bits, e.value, sizeof(e.value));
      entries.push_back(e);
      n++;
    }
  }
  std::fclose(f);
  return n;
}

}  // namespace mem
