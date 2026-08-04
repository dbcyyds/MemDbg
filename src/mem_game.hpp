#pragma once
/**
 * 游戏向 / 自动化
 * - Speedhack（时钟缩放钩子）
 * - 热键（/dev/input EV_KEY）
 * - Lua 5.4 脚本（mem.* API）+ Auto Assemble
 * - Trainer 导出
 */
#include "mem_table.hpp"
#include "mem_lua.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace mem {

// ── Speedhack ─────────────────────────────────────────────
/** mult: 0=关闭, 0.5/1/2/3/5/10…；成功返回 true */
bool speed_set(float mult);
float speed_get();
bool speed_active();
void speed_disable();
const char* speed_status();

// ── 热键 ──────────────────────────────────────────────────
struct HotkeyBind {
  int keycode = 0;          // KEY_* from linux/input-event-codes.h
  char action[32]{};        // freeze_all / unfreeze / speed / script / toggle_ui
  char arg[96]{};           // speed 倍数 / 脚本路径等
  bool enabled = true;
  uint64_t hit_count = 0;
};

bool hotkey_init();
void hotkey_shutdown();
/** 每帧轮询；返回本帧触发的 action 描述（可空） */
const char* hotkey_poll();
void hotkey_clear();
bool hotkey_add(int keycode, const char* action, const char* arg);
bool hotkey_remove(int index);
std::vector<HotkeyBind> hotkey_list();
/** 常用键名 → keycode；失败 -1 */
int hotkey_parse_key(const char* name);
const char* hotkey_key_name(int keycode);

// 脚本：见 mem_lua.hpp（内嵌 Lua 5.4，script_run / mem.*）

// ── Auto Assemble（CE 风格子集）───────────────────────────
/**
 * [ENABLE] / [DISABLE] 段
 * aobscan(sym, XX XX ??)
 * aobscanmodule(sym, libxxx.so, XX ??)
 * alloc(sym, size)
 * label(sym)
 * fullaccess(addr, size)   # 仅记录
 * writebytes(addr_or_sym, XX XX)
 * nop(addr_or_sym, n)
 * registersymbol(sym)
 * unregistersymbol(sym)
 * dealloc(sym)
 * // 行内汇编: 在 alloc 块内用  db / 汇编助记符（经 assemble_line）
 */
bool aa_run(const char* text, bool enable, char* err = nullptr,
            size_t err_cap = 0);
/** 关闭所有由 AA 分配/补丁的资源 */
void aa_disable_all();
const char* aa_status();
int aa_symbol_count();
bool aa_resolve(const char* sym, uintptr_t& out);

// ── Trainer 导出 ──────────────────────────────────────────
struct TrainerMeta {
  char name[64] = "MemDbg Trainer";
  char package[128]{};
  char author[32] = "MemDbg";
  char note[128]{};
};

/** 导出 .trainer 文本（地址表 + 可选启动脚本） */
int trainer_export(const AddressTable& table, const char* path,
                   const TrainerMeta& meta, const char* boot_script = nullptr);
/** 导出可执行 shell 辅助（需已有 memdbg 或用 dd/root 写内存的简易 freeze 循环） */
int trainer_export_sh(const AddressTable& table, const char* path,
                      const TrainerMeta& meta);
/** 加载 .trainer 到地址表，返回条目数 */
int trainer_import(AddressTable& table, const char* path, TrainerMeta* meta_out);

}  // namespace mem
