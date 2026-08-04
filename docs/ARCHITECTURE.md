# MemDbg 架构说明

## 总览

```
┌─────────────────────────────────────────────────────────┐
│  触摸层 touch  (/dev/input → ImGui IO)                    │
├─────────────────────────────────────────────────────────┤
│  UI  float_app  (悬浮球 + 主面板 + 多浮动窗 + 软键盘)      │
├─────────────────────────────────────────────────────────┤
│  渲染  vk_engine  (Vulkan Surface + ImGui Vulkan backend) │
├─────────────────────────────────────────────────────────┤
│  引擎 mem_*  (读写 / 扫描 / 断点 / 反汇编 / Lua / 游戏)    │
├─────────────────────────────────────────────────────────┤
│  内核接口  process_vm_* / ptrace / maps / remote call     │
└─────────────────────────────────────────────────────────┘
```

单进程、**单二进制**部署：所有逻辑静态编入 `vk_imgui_float`，运行时仅依赖系统 `libvulkan` / `libandroid` / `libz` 等。

## 渲染与窗口

1. **ANativeWindow**：通过 `a_native_window_creator` 创建系统级叠加 Surface（需相应权限/厂商支持）。
2. **Vulkan**：Instance → 物理设备 → 逻辑设备 → Swapchain → RenderPass。
3. **Dear ImGui**：`ImGui_ImplVulkan`；每帧 `NewFrame` → 绘制 UI → `Render` → 提交。
4. **字体**：启动时 zlib 解压内嵌 TTF 子集 → `AddFontFromMemoryTTF`；失败再回退系统字体。
5. **配置**：`IniFilename` / `LogFilename` 置空，不写 `imgui.ini`；面板几何等写入 `.mdbg_ui.cfg`。

屏幕旋转：`vkeng::sync_display` + `touch::set_display` 同步宽高与方向。

## 输入

- **触摸**：直接读 `/dev/input/event*`，映射到 `ImGuiIO` 鼠标事件（单击、拖动、长按逻辑在 UI）。
- **软键盘**：`soft_ime` 自绘键盘，写入目标 `char*` 缓冲；可对接剪贴板文件。
- **热键**：`mem_game` 中监听音量等 key 事件（可选）。

## 内存引擎 (`mem_core`)

| 能力 | 实现要点 |
|------|----------|
| 附加 | 记录 pid / 进程名；后续读写走 `process_vm_readv/writev` |
| maps | 解析 `/proc/pid/maps`，区域过滤（匿名/dirty/文件等） |
| 扫描 | 首次 / 再次；类型 I8–I64、Float、Hex、字符串；分页结果 |
| 冻结 | 周期写回冻结列表 |
| 模块 | 基址查询、路径匹配 |

扫描可能较慢的大区域在 UI 上显示状态，避免卡死主线程过久（具体是否异步以实现为准）。

## 调试与断点 (`mem_bp`)

```
          ┌─────────────┐
          │ bp_init     │ 选择后端
          └──────┬──────┘
     ┌───────────┼───────────┐
     ▼           ▼           ▼
  ptrace HW   perf 事件    软 BRK
  (推荐)      (常 ENOSPC)  (SIGTRAP)
```

- **硬件断点/观察点**：`PTRACE_GET/SETREGSET` 调试寄存器；`iov_len` 按真实槽位。
- **软断点**：写入 BRK 指令，命中后恢复单步再插回，避免死循环。
- **控制流**：暂停 / 继续 / 步入 / 步过 / 步出；寄存器与浮点寄存器窗口。
- **remote call / 注入**：在目标线程上下文调用函数；`dlopen` 路径需目标可读 so。

## 分析模块

- **反汇编** `mem_disasm`：ARM64 指令解码 → 指令列表 / 伪 C / xref。
- **符号** `mem_sym`：从 so 刷符号，辅助命名。
- **指针扫描** `mem_ptrscan`：多级偏移链 + 模块 RVA 模板 + 重定位验证。
- **结构** `mem_struct`：按启发式剖析字段类型并跟随指针。

## 脚本 (`mem_lua`)

- 嵌入 **Lua 5.4**，全局表 `mem.*` 暴露读写、扫描、断点、反汇编、变速等。
- 示例按钮写入 `/data/local/tmp/memdbg_example.lua`，运行日志进可伸缩日志框。
- 退出时 `script_shutdown`。

## 游戏向 (`mem_game`)

- **Speedhack**：HARD（钩时间相关，风险高）/ SOFT（仅加速冻结写回）。
- **Auto Assemble**：简化 AA 脚本 ENABLE/DISABLE。
- **Trainer**：地址表 + 元数据导出/导入。

## UI 信息架构

```
悬浮球 ──单击──► 主面板
                  ├ 进程 / 搜索 / 地址 / 分析 / 调试 / 自动化
                  ├ 子导航芯片（页内）
                  └ 右/下/角 缩放；标题拖动
         多浮动窗（反汇编 / 伪C / 命中日志…）独立拖缩
```

面板尺寸强制固定 + 内容区 `BeginChild` 滚动，避免 ImGui 被内容撑开导致“只能横向缩放”的错觉。

## 配置与清理

| 路径 | 用途 | 生命周期 |
|------|------|----------|
| `/data/local/tmp/.mdbg_ui.cfg` | 球/面板位置、Tab、过滤等 | **保留** |
| `/data/local/tmp/memdbg_*` | dump / 示例 / 导出 | 退出/启动清理 |
| `/data/local/tmp/.mdbg_*` | dex / 缓存 | 清理（除 ui.cfg） |
| `/data/local/tmp/.m_*` | 加密包解出缓存 | 可清理 |

## 安全与权限

- 必须 **root**：ptrace、process_vm、读 input、叠加层。
- 目标进程可能有反调试；硬件断点槽位有限。
- 注入 / HARD 变速可能导致目标崩溃，UI 有提示。
- 本工具面向研究与授权调试；请遵守当地法律与应用条款。

## 数据流（搜索示例）

```
UI 输入类型/值
    → mem::scan_first / scan_next
    → 结果缓冲
    → UI 分页展示
    → 用户冻结 / 加入地址表 / 下断点 / 反汇编
```

## 扩展建议

1. 新 UI 页：在 `Tab` 枚举 + `draw_panel` switch + `tab_xxx`。
2. 新 mem API：`mem_*.hpp` 声明 → UI 按钮 → 可选 Lua 绑定。
3. 新打包：改 `pack_obf_sh.py` 的 stub/密钥派生，保持 `tail` 剥离载荷以避免 ARG_MAX。
