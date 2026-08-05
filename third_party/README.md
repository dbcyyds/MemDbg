# third_party

| 目录 | 说明 |
|------|------|
| `imgui/` | Dear ImGui（本仓库 vendored 拷贝，或 `git clone https://github.com/ocornut/imgui.git third_party/imgui`） |
| `lua-5.4.7/` | Lua 5.4.7 官方源码布局（从 https://www.lua.org/ftp/lua-5.4.7.tar.gz 解压） |

若 clone 后缺少目录：

```bash
# ImGui
git clone --depth 1 https://github.com/ocornut/imgui.git third_party/imgui

# Lua 5.4.7
curl -L https://www.lua.org/ftp/lua-5.4.7.tar.gz | tar -xz -C third_party
```
