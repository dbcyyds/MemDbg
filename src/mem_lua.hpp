#pragma once
/**
 * 内嵌 Lua 5.4 脚本引擎
 * 全局表 mem.* 提供读写/冻结/AOB/变速等
 */
#include <cstdint>
#include <cstddef>

namespace mem {

/** 执行一段 Lua 源码；失败时 err 写入错误 */
bool script_run(const char* text, char* err = nullptr, size_t err_cap = 0);
/** 执行 .lua 文件 */
bool script_run_file(const char* path, char* err = nullptr, size_t err_cap = 0);
/** 脚本 print 输出缓冲 */
const char* script_log();
void script_log_clear();
/** 最近一次 mem.aob 命中地址 */
uintptr_t script_last_aob();

/** 关闭 Lua 状态（进程退出时） */
void script_shutdown();

}  // namespace mem
