# MemDbg Lua API 摘要

嵌入 **Lua 5.4**，脚本中通过全局表 `mem` 调用。UI 路径：**自动化 → Lua**。

示例会覆盖写入：`/data/local/tmp/memdbg_example.lua`。

## 基础

| API | 说明 |
|-----|------|
| `mem.is_attached()` | 是否已附加 |
| `mem.pid()` / `mem.name()` | 目标 pid / 名 |
| `mem.help()` | 打印帮助到日志 |
| `mem.sleep(ms)` | 休眠毫秒 |
| `mem.status()` | 调试/断点状态串 |

## 读写

```lua
mem.write_i32(addr, v)  mem.read_i32(addr)
mem.write_f32 / read_f32 / write_f64 / read_f64
mem.write_str / read_str
mem.write_hex(addr, "48 69 00")
mem.read_bytes(addr, n)  -- 返回 Lua 字符串
```

## 扫描 / 冻结

```lua
mem.scan_first(value, type, mode, region)
mem.scan_next(value, type, mode)
mem.scan_wait(timeout_ms?)
mem.scan_count()  mem.scan_status()
mem.scan_results(limit)
mem.freeze(addr, type, value)  mem.unfreeze(addr)  mem.clear_frozen()
mem.export_results(path)  mem.dump(addr, size, path)
```

类型示例：`"i32"` `"f32"` `"utf8"` …；模式：`"exact"` `"unchanged"` …；区域：`"anon"` 等。

## 指针 / 结构 / 模块

```lua
mem.ptrscan(target, depth, max_off)
mem.ptrscan_results(limit)
mem.struct_dissect(base, nfields)
mem.module_base("libc.so")  -- start, end
mem.modules(filter?)
mem.maps(writable_only?, filter?)
```

## 调试

```lua
mem.pause()  mem.resume()  mem.step()  mem.step_over()
mem.regs()   -- .pc .sp .lr .x0 ...
mem.bp_init()
mem.bp_set(addr, "exec"|"write"|..., size)
mem.soft_bp(addr, cond?, oneshot?)
mem.stack(n)  mem.trace(n)
mem.threads()
```

## 补丁 / 注入 / 符号

```lua
mem.nop(addr, n_insns)
mem.assemble("mov x0, #0")  -- words, hex
mem.patch_asm(addr, asm)
mem.find_dlopen()
mem.remote_call(fn, {args...})
mem.inject_so(path)
mem.sym_refresh()  mem.sym_find(name)  mem.sym_name(addr)
```

## 变速 / AA

```lua
mem.speed(2.0)       -- 或 "off"
mem.aa(script, enable_bool)
```

## 反汇编

```lua
mem.disasm(addr, count)
mem.pseudo_c(addr, max_insns)
```

## 进程列表

```lua
mem.list_procs(filter?)
```

## 注意

- 写代码地址 / 注入 / HARD 变速可能导致目标崩溃，请在测试环境操作。
- 地址请换成真实指针；示例中 `0x0` 仅为占位。
- 完整列表以 `mem.help()` 及 `src/mem_lua.cpp` 为准。
