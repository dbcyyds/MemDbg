#pragma once
/**
 * Vulkan 引擎封装：Android 悬浮层 ANativeWindow + ImGui Vulkan 后端
 * 使用系统 libvulkan（Adreno GPU），透明清屏。
 */
#include <cstdint>

struct ANativeWindow;

namespace vkeng {

struct Config {
  const char* layer_name = "VkImGuiFloat";
  bool skip_screenshot = false;  // true = 防截图
  int min_image_count = 2;
};

struct Display {
  int width = 1080;   // 逻辑宽
  int height = 1920;  // 逻辑高
  int side = 1920;    // 正方形 surface 边长 max(w,h)
  int orient = 0;     // 0/1/2/3
};

bool init(const Config& cfg = {});
void shutdown();

/** 刷新横竖屏信息，必要时重建 swapchain */
void sync_display();
Display display();

/** 帧：begin → 用户画 ImGui → end(present) */
void new_frame();
void end_frame();  // Render + submit + present；clear 为全透明

/** 内部窗口尺寸变化或 swapchain 过期时调用 */
void request_rebuild();

ANativeWindow* native_window();
bool ok();

/** 用户纹理（进程图标等）：RGBA8 → ImGui 可用 ImTextureID */
struct Texture {
  uint64_t id = 0;  // ImTextureID (= VkDescriptorSet)
  int w = 0;
  int h = 0;
  bool valid() const { return id != 0; }
};

/** 上传 RGBA8 像素；失败返回 invalid。调用方勿释放 GPU 资源，用 destroy_texture */
Texture create_texture_rgba(const uint8_t* rgba, int w, int h);
void destroy_texture(Texture& tex);
void destroy_all_textures();

}  // namespace vkeng
