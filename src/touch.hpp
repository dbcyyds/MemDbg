#pragma once
/**
 * 读 /dev/input 触摸（对齐 lostgo TouchHelperA 读法，无 ImGui）
 */
namespace touch {

struct State {
  float x = 0.f;
  float y = 0.f;
  bool down = false;
  bool just_pressed = false;
  bool just_released = false;
};

bool init(float screen_w, float screen_h);
void shutdown();

/** 横竖屏变化时更新逻辑分辨率与 orientation(0/1/2/3) */
void set_display(float screen_w, float screen_h, int orientation);

State snapshot();

}  // namespace touch

