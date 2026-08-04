# MemDbg 开发指南

## 环境要求

| 组件 | 说明 |
|------|------|
| 主机 | Android **aarch64**，**root**（Termux 或同类） |
| 编译器 | `clang` / `clang++`（NDK r29 或 Termux 自带） |
| C++ | C++20 |
| 依赖库 | `libvulkan.so`、`libandroid.so`、`liblog.so`、`libz.so`（系统路径） |
| 静态 | `libc++_static.a`（NDK/Termux） |
| 可选 | `upx`（打包压缩）、`python3` + `fonttools`（重建字体子集）、JDK + d8（重建 icon dex） |

建议在 **真机 root** 上直接编译运行，便于联调触摸与 Vulkan 叠加层。

## 获取源码

```bash
git clone https://github.com/dbcyyds/MemDbg.git
cd MemDbg
# 若使用 submodule 方式拉取 imgui（见 third_party/README）
# git submodule update --init --recursive
```

仓库自带：

- `third_party/imgui` — Dear ImGui 核心 + Vulkan backend（精简拷贝）
- `third_party/lua-5.4.7` — Lua 5.4 源码
- `data/memdbg_cjk_subset.ttf` — 内嵌用中文字体子集

## 一键构建

```bash
bash build.sh
```

构建步骤概要：

1. 嵌入 `icon_dump.dex` → `src/icon_dump_dex_embed.h`（若 `out/icon_dump.dex` 存在）
2. 嵌入 CJK 字体 zlib → `src/font_embed.h`（由 `data/memdbg_cjk_subset.ttf` 生成）
3. 编译 `src/*.cpp`、ImGui、Lua
4. 静态链接 C++ 运行时，动态链接 Vulkan/Android/z
5. 修补 ELF rpath，仅保留系统库路径（`tools/fix_rpath.py`）

产物：

```
out/vk_imgui_float          # 主程序（单文件）
out/icon_dump.dex           # 可选：图标 dump（也会内嵌进主程序）
```

### 单独重编某模块

```bash
rm -f out/obj/float_app.o
bash build.sh
```

### 运行

```bash
su -c "$(pwd)/out/vk_imgui_float"
su -c "$(pwd)/out/vk_imgui_float --pid 12345"
su -c "$(pwd)/out/vk_imgui_float --secure"
su -c "$(pwd)/out/vk_imgui_float --help"
```

快捷脚本：`bash run.sh`（内部 `su -c`）。

## 目录结构

```
MemDbg/
├── README.md                 # 项目总览
├── docs/
│   ├── DEVELOPMENT.md        # 本文件
│   ├── ARCHITECTURE.md       # 架构说明
│   └── LUA_API.md            # Lua API 摘要
├── build.sh                  # 主构建脚本
├── run.sh
├── data/
│   └── memdbg_cjk_subset.ttf # 中文字体子集（构建时嵌入）
├── include/
│   └── a_native_window_creator.h
├── src/                      # 核心源码
│   ├── main.cpp
│   ├── float_app.*           # UI 面板 / 导航 / 配置
│   ├── vk_engine.*           # Vulkan + ImGui 初始化 / 字体
│   ├── touch.*               # /dev/input 触摸
│   ├── soft_ime.*            # 内嵌软键盘
│   ├── mem_core.*            # 附加 / 读写 / 扫描 / maps
│   ├── mem_table.*           # 地址表
│   ├── mem_ptrscan.*         # 指针扫描
│   ├── mem_struct.*          # 结构剖析
│   ├── mem_disasm.* / mem_asm.* / mem_sym.*
│   ├── mem_bp.*              # 硬件/软断点 / 调试控制
│   ├── mem_game.*            # 变速 / 热键 / AA / Trainer
│   ├── mem_lua.*             # Lua 绑定
│   └── mem_icon.*            # 进程图标
├── java_ime/
│   ├── IconDump.java         # 图标提取（dex 内嵌）
│   └── build_icon_dump.sh
├── tools/
│   ├── pack_obf_sh.py        # UPX + 类 dbc21 加密 sh 打包
│   ├── mdbg_dec.c            # 加密载荷解密小工具
│   ├── fix_rpath.py
│   └── test_*.cpp / mem_selftest.cpp
└── third_party/
    ├── imgui/
    └── lua-5.4.7/
```

## 编码约定

- **语言**：C++20；UI 字符串以简体中文为主。
- **内存引擎命名空间**：`mem::`；UI：`float_app::`；渲染：`vkeng::`。
- **错误处理**：对用户可见操作用 `ui.toast` / 状态栏；底层返回 `bool` 或错误串。
- **线程**：UI 主线程为主；扫描等可异步，注意与 ptrace 调试互斥。
- **路径**：运行时临时文件默认 `/data/local/tmp/`；UI 配置 `.mdbg_ui.cfg` **退出时保留**，其它残留自动清理。

## 修改 UI

主入口：`src/float_app.cpp`

- 导航 Tab：`enum class Tab` + `draw_panel` / 各 `tab_*`
- 主题色：`namespace theme`
- 面板拖动/缩放：`update_main_panel_geom`（右/下/角）
- 配置存盘：`save_ui_config` / `load_ui_config` → `/data/local/tmp/.mdbg_ui.cfg`

样式在 `apply_kawaii_imgui_style()`；字体在 `vk_engine.cpp`（内嵌优先）。

## 修改内存引擎

| 需求 | 文件 |
|------|------|
| process_vm 读写、扫描 | `mem_core.*` |
| 地址表冻结 | `mem_table.*` |
| 断点 / 单步 / 寄存器 | `mem_bp.*` |
| 反汇编 / 伪 C | `mem_disasm.*`、`mem_asm.*` |
| 指针链 | `mem_ptrscan.*` |
| 结构字段 | `mem_struct.*` |
| Lua API | `mem_lua.*` |
| 变速 / AA / 注入 | `mem_game.*` |

硬件断点注意：`PTRACE_SETREGSET` 的 `iov_len` 必须与真实槽位数匹配（exec/watch 不同）。

## 字体子集重建

换源字体或补字时：

```bash
# 需: pip install fonttools
# 从系统 TTF 按 ImGui 简体常用表子集（示例脚本逻辑见历史 tools）
# 输出到 data/memdbg_cjk_subset.ttf 后：
bash build.sh   # 自动生成 font_embed.h 并链接
```

`stb_truetype` **无法**加载 CFF/OTTO 的 `NotoSansCJK.ttc`，请使用 TrueType（如 ZUKChinese.ttf 子集）。

## 图标 dex 重建

```bash
cd java_ime
bash build_icon_dump.sh   # 生成 out/icon_dump.dex
cd ..
bash build.sh             # 重新嵌入
```

## 加密单文件打包（dbc21 风格）

```bash
bash build.sh
python3 tools/pack_obf_sh.py \
  -i out/vk_imgui_float \
  -o out/dbc21_.sh \
  --install /data/local/dbc21_.sh
```

格式：第 1 行引导 shell + 后续 XOR 加密（UPX+gzip）载荷。需 root + Magisk/KSU busybox。

```bash
su -c 'sh /data/local/dbc21_.sh --help'
```

## 调试建议

```bash
# 日志直接看 stdout
su -c './out/vk_imgui_float' 2>&1 | tee /data/local/tmp/mdbg.log

# 字体是否内嵌成功
grep '\[font\]' /data/local/tmp/mdbg.log

# 无 UI 引擎自测（若已编 tools）
# su -c './out/mem_selftest <pid>'
```

常见问题：

| 现象 | 排查 |
|------|------|
| 中文方块 | 确认 `[font] embedded CJK subset`；重建 font_embed |
| 触摸无效 | root 读 `/dev/input`；多点触控设备过滤 |
| 附加失败 | SELinux / 目标防护；确认 PID |
| HW BP ENOSPC | 槽位已满；检查 iov 长度；可改软断点 |
| 叠加层不显示 | Vulkan 扩展 / 权限 / 屏幕方向 `sync_display` |

## 提交与发布

```bash
git status
git add -A
git commit -m "feat: ..."
git push origin main
```

请勿提交：`out/` 二进制、`font_embed.h`（可生成）、个人路径密钥、目标进程 dump。

## License

见根目录 [LICENSE](../LICENSE)。第三方 ImGui / Lua 遵循各自 MIT 许可。
