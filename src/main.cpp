/**
 * MemDbg — Android 悬浮内存调试工具 UI
 * Vulkan GPU + Dear ImGui + 动画悬浮球/面板
 *
 * 用法（root）:
 *   ./vk_imgui_float
 *   ./vk_imgui_float --secure
 */
#include "float_app.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);

  float_app::Config cfg;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--secure") == 0 ||
        std::strcmp(argv[i], "-s") == 0)
      cfg.skip_screenshot = true;
    else if (std::strcmp(argv[i], "--demo-ui") == 0)
      cfg.demo_ui = true;
    else if (std::strncmp(argv[i], "--pid=", 6) == 0)
      cfg.debug_pid = atoi(argv[i] + 6);
    else if (std::strcmp(argv[i], "--pid") == 0 && i + 1 < argc)
      cfg.debug_pid = atoi(argv[++i]);
    else if (std::strcmp(argv[i], "-h") == 0 ||
             std::strcmp(argv[i], "--help") == 0) {
      std::printf(
          "MemDbg — CE 风格内存调试（单文件 / root）\n"
          "  --secure / -s     防截图\n"
          "  --pid N           启动时附加 PID（0=不自动附加）\n"
          "  --demo-ui         自动展开并轮播各界面（截图用）\n"
          "  需要 root\n");
      return 0;
    }
  }

  std::printf("MemDbg starting...\n");
  return float_app::run(cfg);
}
