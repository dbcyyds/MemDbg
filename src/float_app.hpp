#pragma once
/**
 * MemDbg — 内存调试工具 UI 壳
 * 悬浮球 / 面板 / 按钮均带动画；当前为 UI 先行，逻辑可后续挂载。
 */
namespace float_app {

struct Config {
  int frame_ms = 16;   // 展开/交互动画
  int idle_ms = 33;    // 仅悬浮球呼吸动画 ~30fps
  bool skip_screenshot = false;
  int debug_pid = 0;        // 0=不自动附加；可用 --pid N
  bool demo_ui = false;     // 自动展开并轮播各 Tab（便于截图）
  int demo_tab_ms = 1800;   // 每个 Tab 停留毫秒
};

int run(const Config& cfg = {});

}  // namespace float_app
