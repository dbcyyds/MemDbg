# MemDbg

**Android 上的 CE 风格内存调试工具** — Vulkan 全屏叠加 + Dear ImGui + root 内存引擎。

单文件二进制、内嵌中文字体与图标 dex，换设备开箱可显示中文。需 **root**。

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Android%20aarch64-green.svg)](#)
[![UI](https://img.shields.io/badge/UI-Vulkan%20%2B%20ImGui-purple.svg)](#)

---

## 目录

- [特性](#特性)
- [截图与界面](#截图与界面)
- [快速开始](#快速开始)
- [使用流程](#使用流程)
- [命令行参数](#命令行参数)
- [项目结构](#项目结构)
- [编译](#编译)
- [打包分发](#打包分发)
- [配置与清理](#配置与清理)
- [文档](#文档)
- [已知限制](#已知限制)
- [License](#license)

---

## 特性

| 模块 | 能力 |
|------|------|
| **进程** | 列表 / 模糊过滤 / APK 图标 / 腾讯过滤 / 附加·断开 |
| **搜索** | 精确·区间·变化·模糊；I8–I64 / Float / Hex / UTF-8 / U16；区域过滤；结果分页与冻结 |
| **地址表** | CE 风格 Cheat Table；存盘加载；批量冻结 |
| **浏览** | Hex 视图；数值 ±1；跳转反汇编 / 观察点 |
| **指针** | 多级扫描；模块+RVA 模板；重定位验证 |
| **结构** | 自动剖析字段；跟随指针；存盘 |
| **调试** | 暂停 / 步入·过·出；硬件断点·观察点；软断点（条件）；寄存器·浮点；栈 / Trace；线程；remote call；so 注入 |
| **自动化** | Speedhack（HARD/SOFT）；全局热键；**Lua 5.4 全工具 API**；Auto Assemble；Trainer 导入导出 |
| **输入** | 内嵌软键盘；触控拖动缩放面板（右/下/角） |
| **部署** | 单 ELF；中文字体内嵌；可选 UPX + 类 dbc21 加密 sh 任意路径运行 |

---

## 截图与界面

- **悬浮球**：单击开合面板；拖动换位置。
- **主面板**：2×3 导航（进程 / 搜索 / 地址 / 分析 / 调试 / 自动化）；子页芯片导航。
- **缩放**：拖 **右边**改宽、**底边**改高、**右下角**双轴；内容超出时内部滚动。
- **多窗口**：反汇编 / 伪 C / 命中日志等可独立拖动缩放。

---

## 快速开始

### 运行已编译二进制

```bash
su -c /path/to/vk_imgui_float
# 或加密包
su -c 'sh /data/local/dbc21_.sh'
```

### 从源码编译（Android aarch64 + root/Termux）

```bash
git clone https://github.com/dbcyyds/MemDbg.git
cd MemDbg
bash build.sh
su -c "$(pwd)/out/vk_imgui_float"
```

依赖：`clang++`、NDK/Termux `libc++_static`、系统 `libvulkan` / `libandroid` / `libz`。详见 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)。

---

## 使用流程

1. **进程** → 刷新列表 → 点选 App → 附加  
2. **搜索** → 选类型与条件 → 首次 / 再次搜索 → 结果里冻结或加入地址表  
3. **地址表** 保存到 `/data/local/tmp/memdbg_*`  
4. **分析 → 指针** 扫稳定链；**结构** 剖析对象布局  
5. **调试** 下断点 / 单步 / 看寄存器；需要时 remote call 或注入 so  
6. **自动化 → Lua** 点示例生成脚本 → 运行 → 查看日志  

默认**不会**自动附加进程（除非 `--pid`）。

---

## 命令行参数

```
MemDbg — CE 风格内存调试（单文件 / root）
  --secure / -s     防截图（窗口安全标志）
  --pid N           启动时附加 PID
  --demo-ui         自动展开并轮播界面（截图/演示）
  -h / --help       帮助
```

---

## 项目结构

```
src/                 引擎 + UI 源码
  float_app.*        主界面
  vk_engine.*        Vulkan / ImGui / 内嵌字体
  mem_*.*            内存 / 断点 / 反汇编 / Lua / …
  soft_ime.*         软键盘
  touch.*            触摸
docs/                开发 / 架构 / Lua API
data/                中文字体子集（构建嵌入）
java_ime/            IconDump 等 Java 辅助
tools/               打包、自测、rpath
third_party/         imgui、lua-5.4.7
build.sh             一键构建
```

更细的模块说明见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

---

## 编译

```bash
bash build.sh
```

构建时会：

1. 嵌入 `icon_dump.dex`（若存在）
2. 将 `data/memdbg_cjk_subset.ttf` 压成 zlib 写入 `src/font_embed.h`
3. 编译链接为 `out/vk_imgui_float`
4. 修正 RPATH 为仅系统库

---

## 打包分发

```bash
bash build.sh
python3 tools/pack_obf_sh.py -i out/vk_imgui_float -o out/dbc21_.sh --install /data/local/dbc21_.sh
```

- UPX 压缩 ELF  
- 单行 shell 引导 + 加密二进制载荷（兼容 `/system/bin/sh` + Magisk busybox）  
- 任意路径：`su -c 'sh /path/to/dbc21_.sh'`

---

## 配置与清理

| 路径 | 说明 |
|------|------|
| `/data/local/tmp/.mdbg_ui.cfg` | 面板/过滤等 UI 配置，**保留** |
| `/data/local/tmp/memdbg_*` | dump、示例脚本、导出等，启动/退出清理 |
| ImGui `imgui.ini` | **已禁用**自动保存，避免换目录/换设备异常 |

中文字体已内嵌，**不依赖**目标机 `/system/fonts` 是否含 ZUKChinese 等。

---

## 文档

| 文档 | 内容 |
|------|------|
| [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) | 环境、编译、目录约定、调试、发布 |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 分层架构、数据流、扩展点 |
| [docs/LUA_API.md](docs/LUA_API.md) | Lua `mem.*` API 摘要 |

---

## 已知限制

- 以 **ARM64** 为主；反汇编非完整指令集  
- so 注入依赖 ptrace 与可解析的 `dlopen`  
- 硬件观察点对「调试器自身 process_vm 写入」可能采不到（内核差异）  
- HARD 变速 / 随意补丁可能导致目标崩溃  
- 需 root 与读 `/dev/input` 的权限  
- 大进程全图扫描可能较慢  

---

## 免责声明

仅供安全研究、逆向学习与**自有或已授权**软件调试。请遵守所在地法律与软件许可协议。作者不对滥用或数据丢失负责。

---

## License

[MIT](LICENSE) © DBC ([@dbcyyds](https://github.com/dbcyyds))

第三方：Dear ImGui、Lua 5.4 均为 MIT。
