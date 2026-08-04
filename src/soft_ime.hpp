#pragma once
/**
 * MemDbg 内嵌软键盘（无系统 IME / 无宿主 APK）
 * - 十六进制 0–F（1～16 进制数字）
 * - 26 英文字母
 * - 汇编助记符 / 寄存器 / 常用符号
 * 界面：底部弹出、按键缩放与波纹动画
 */
#include <cstddef>

namespace soft_ime {

enum class Mode : int {
  Hex = 0,  // 0-9 A-F
  Abc,      // A-Z / a-z
  Asm,      // 汇编
  COUNT
};

/** 打开键盘并绑定缓冲（实时写入 buf） */
bool open(char* buf, size_t cap, const char* title, Mode mode = Mode::Abc);

void close();
bool is_open();

/** 是否本帧点了「完成」关闭 */
bool just_confirmed();
bool just_cancelled();

/**
 * 每帧在 ImGui::NewFrame() 之后调用：
 * 更新动画并绘制键盘（覆盖在最上层）
 */
void update_and_draw(float dt);

}  // namespace soft_ime
