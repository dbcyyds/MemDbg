#include "soft_ime.hpp"

#include "vk_engine.hpp"

#include "imgui.h"
#include "imgui_internal.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

namespace soft_ime {
namespace {

// ── 工具 ──────────────────────────────────────────────────
inline float clampf(float v, float a, float b) {
  return v < a ? a : (v > b ? b : v);
}
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float ease_out_cubic(float t) {
  t = clampf(t, 0.f, 1.f);
  float u = 1.f - t;
  return 1.f - u * u * u;
}
inline ImU32 col4(float r, float g, float b, float a) {
  return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255),
                  (int)(clampf(a, 0.f, 1.f) * 255));
}
inline ImVec4 operator+(ImVec4 a, ImVec4 b) {
  return ImVec4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w);
}
inline ImVec4 operator*(ImVec4 a, float s) {
  return ImVec4(a.x * s, a.y * s, a.z * s, a.w * s);
}
inline ImVec4 lerp4(ImVec4 a, ImVec4 b, float t) {
  return a * (1.f - t) + b * t;
}
inline bool pt_in(ImVec2 p, float x0, float y0, float x1, float y1) {
  return p.x >= x0 && p.x < x1 && p.y >= y0 && p.y < y1;
}

// ── 主题 ──────────────────────────────────────────────────
namespace th {
constexpr ImVec4 panel(1.00f, 0.96f, 0.97f, 0.97f);
constexpr ImVec4 panel_edge(1.00f, 0.65f, 0.78f, 0.55f);
constexpr ImVec4 key(1.00f, 0.88f, 0.91f, 1.f);
constexpr ImVec4 key_hi(1.00f, 0.80f, 0.86f, 1.f);
constexpr ImVec4 key_fn(0.95f, 0.82f, 0.95f, 1.f);
constexpr ImVec4 accent(1.00f, 0.55f, 0.72f, 1.f);
constexpr ImVec4 accent_dim(1.00f, 0.70f, 0.82f, 1.f);
constexpr ImVec4 ok(0.60f, 0.90f, 0.75f, 1.f);
constexpr ImVec4 del(1.00f, 0.60f, 0.68f, 1.f);
constexpr ImVec4 text(0.38f, 0.28f, 0.35f, 1.f);
constexpr ImVec4 muted(0.72f, 0.58f, 0.65f, 1.f);
constexpr ImVec4 preview_bg(1.00f, 0.93f, 0.95f, 0.98f);
constexpr ImVec4 title_bar(1.00f, 0.78f, 0.86f, 1.f);
}  // namespace th

enum ResizeMask : int {
  R_None = 0,
  R_L = 1,
  R_R = 2,
  R_T = 4,
  R_B = 8,
};

struct KeyAnim {
  float hover = 0.f;
  float press = 0.f;
  float flash = 0.f;
};

struct KeyDef {
  const char* label = "";
  const char* insert = nullptr;
  float width = 1.f;
  // 0 text, 1 del, 2 clear, 3 space, 4 done, 5 cancel, 6 shift,
  // 7 系统剪贴板, 8 历史剪贴
  int kind = 0;
};

enum class AsmPage : int { Mnem = 0, Reg, Sym, COUNT };

struct State {
  bool open = false;
  bool want_open = false;
  float anim = 0.f;
  float time = 0.f;

  char* target = nullptr;
  size_t cap = 0;
  char title[48] = "输入";
  Mode mode = Mode::Abc;
  AsmPage asm_page = AsmPage::Mnem;
  bool shift = true;

  char draft[512]{};
  size_t draft_len = 0;

  // 系统剪贴板 + 粘贴历史（最近 12 条）
  char clip_buf[512]{};
  char clip_hist[12][256]{};
  int clip_hist_n = 0;
  int clip_hist_i = 0;  // 循环查看
  char clip_status[64] = "";

  bool confirmed = false;
  bool cancelled = false;

  KeyAnim keys[128]{};
  int key_n = 0;
  float mode_slide = 0.f;
  float asm_slide = 0.f;

  // 几何
  bool geom_init = false;
  float x = 0.f, y = 0.f;
  float w = 400.f, h = 280.f;

  // 拖 / 缩：锚点模型（不用累积 delta，触摸更稳）
  bool dragging = false;
  bool drag_pending = false;  // 按在键盘上，尚未超过阈值
  int resizing = R_None;
  float grab_mx = 0.f, grab_my = 0.f;  // 按下时鼠标
  float grab_x = 0.f, grab_y = 0.f;    // 按下时窗口
  float grab_w = 0.f, grab_h = 0.f;
  bool mouse_was_down = false;

  // 拖动中 / 缩放中：屏蔽按键
  bool interacting() const {
    return dragging || resizing != R_None;
  }
};

State g;

constexpr float kMinW = 240.f;
constexpr float kMinH = 170.f;
constexpr float kEdge = 16.f;
constexpr float kCorner = 40.f;  // 右下角热区
constexpr float kTitleH = 36.f;
constexpr float kDragThresh = 10.f;  // 超过此位移才算拖动，轻点仍可按键

void sync_target_from_draft() {
  if (!g.target || g.cap < 2) return;
  std::snprintf(g.target, g.cap, "%s", g.draft);
}

void load_draft_from_target() {
  g.draft[0] = '\0';
  g.draft_len = 0;
  if (g.target && g.target[0]) {
    std::snprintf(g.draft, sizeof(g.draft), "%s", g.target);
    g.draft_len = std::strlen(g.draft);
  }
}

void append_text(const char* s) {
  if (!s || !s[0]) return;
  size_t n = std::strlen(s);
  if (g.draft_len + n >= sizeof(g.draft) - 1) return;
  if (g.cap > 1 && g.draft_len + n >= g.cap - 1) return;
  std::memcpy(g.draft + g.draft_len, s, n);
  g.draft_len += n;
  g.draft[g.draft_len] = '\0';
  sync_target_from_draft();
}

void backspace() {
  if (g.draft_len == 0) return;
  do {
    g.draft_len--;
  } while (g.draft_len > 0 &&
           ((unsigned char)g.draft[g.draft_len] & 0xC0) == 0x80);
  g.draft[g.draft_len] = '\0';
  sync_target_from_draft();
}

void clear_all() {
  g.draft[0] = '\0';
  g.draft_len = 0;
  sync_target_from_draft();
}

void clamp_geom() {
  auto d = vkeng::display();
  const float sw = (float)d.width, sh = (float)d.height;
  g.w = clampf(g.w, kMinW, sw - 4.f);
  g.h = clampf(g.h, kMinH, sh - 4.f);
  g.x = clampf(g.x, 0.f, sw - g.w);
  g.y = clampf(g.y, 0.f, sh - g.h);
}

void ensure_default_geom() {
  auto d = vkeng::display();
  const float sw = (float)d.width, sh = (float)d.height;
  const float shorter = std::min(sw, sh);
  const bool tablet = shorter >= 700.f || sw >= 900.f;

  if (!g.geom_init) {
    g.w = tablet ? clampf(sw * 0.42f, 340.f, 480.f)
                 : clampf(sw - 28.f, 280.f, 400.f);
    g.h = tablet ? clampf(sh * 0.30f, 210.f, 300.f)
                 : clampf(sh * 0.36f, 210.f, 320.f);
    g.x = (sw - g.w) * 0.5f;
    g.y = sh - g.h - 20.f;
    g.geom_init = true;
  }
  clamp_geom();
}

// ── 命中：边角缩放 / 标题拖动 ─────────────────────────────
int hit_resize(ImVec2 m, float x, float y, float w, float h) {
  const float c = kCorner;
  const float e = kEdge;
  // 角（右下最大）
  if (pt_in(m, x + w - c, y + h - c, x + w + 4.f, y + h + 4.f))
    return R_R | R_B;
  if (pt_in(m, x - 4.f, y + h - c, x + c, y + h + 4.f)) return R_L | R_B;
  if (pt_in(m, x + w - c, y - 4.f, x + w + 4.f, y + c)) return R_R | R_T;
  if (pt_in(m, x - 4.f, y - 4.f, x + c, y + c)) return R_L | R_T;
  // 边
  if (pt_in(m, x + w - e, y + c, x + w + 4.f, y + h - c)) return R_R;
  if (pt_in(m, x - 4.f, y + c, x + e, y + h - c)) return R_L;
  if (pt_in(m, x + c, y + h - e, x + w - c, y + h + 4.f)) return R_B;
  if (pt_in(m, x + c, y - 4.f, x + w - c, y + e)) return R_T;
  return R_None;
}

// 关闭按钮区域
bool hit_close(ImVec2 m, float x, float y, float w) {
  return pt_in(m, x + w - 42.f, y + 2.f, x + w - 2.f, y + kTitleH);
}

// 键盘面板任意位置（整窗可拖，排除关闭钮）
bool hit_panel(ImVec2 m, float x, float y, float w, float h) {
  if (!pt_in(m, x, y, x + w, y + h)) return false;
  if (hit_close(m, x, y, w)) return false;
  return true;
}

void begin_grab(ImVec2 m) {
  g.grab_mx = m.x;
  g.grab_my = m.y;
  g.grab_x = g.x;
  g.grab_y = g.y;
  g.grab_w = g.w;
  g.grab_h = g.h;
}

/**
 * 输入处理放在绘制之前，同一帧位置立即生效。
 * - 边角：缩放
 * - 键盘任意处（除关闭）：按住滑动即可拖动
 * - 轻点（位移 < 阈值）仍可触发按键
 */
void process_pointer() {
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 m = io.MousePos;
  const bool down = io.MouseDown[0];
  const bool pressed = down && !g.mouse_was_down;
  const bool released = !down && g.mouse_was_down;

  auto d = vkeng::display();
  const float sw = (float)d.width, sh = (float)d.height;

  if (released) {
    g.dragging = false;
    g.drag_pending = false;
    g.resizing = R_None;
  }

  if (pressed && g.anim > 0.5f) {
    int mask = hit_resize(m, g.x, g.y, g.w, g.h);
    if (mask != R_None) {
      g.resizing = mask;
      g.dragging = false;
      g.drag_pending = false;
      begin_grab(m);
    } else if (hit_panel(m, g.x, g.y, g.w, g.h)) {
      // 整板可拖：先挂起，移动超阈值再正式拖（避免点按键被抢走）
      g.resizing = R_None;
      g.dragging = false;
      g.drag_pending = true;
      begin_grab(m);
    }
  }

  if (down && g.drag_pending && !g.dragging) {
    float dx = m.x - g.grab_mx;
    float dy = m.y - g.grab_my;
    if (dx * dx + dy * dy >= kDragThresh * kDragThresh) {
      g.dragging = true;
      g.drag_pending = false;
    }
  }

  if (down && g.dragging) {
    g.x = g.grab_x + (m.x - g.grab_mx);
    g.y = g.grab_y + (m.y - g.grab_my);
    clamp_geom();
  } else if (down && g.resizing != R_None) {
    const float dx = m.x - g.grab_mx;
    const float dy = m.y - g.grab_my;
    float nx = g.grab_x, ny = g.grab_y, nw = g.grab_w, nh = g.grab_h;

    if (g.resizing & R_L) {
      nx = g.grab_x + dx;
      nw = g.grab_w - dx;
    }
    if (g.resizing & R_R) nw = g.grab_w + dx;
    if (g.resizing & R_T) {
      ny = g.grab_y + dy;
      nh = g.grab_h - dy;
    }
    if (g.resizing & R_B) nh = g.grab_h + dy;

    if (nw < kMinW) {
      if (g.resizing & R_L) nx = g.grab_x + g.grab_w - kMinW;
      nw = kMinW;
    }
    if (nh < kMinH) {
      if (g.resizing & R_T) ny = g.grab_y + g.grab_h - kMinH;
      nh = kMinH;
    }
    if (nw > sw - 4.f) nw = sw - 4.f;
    if (nh > sh - 4.f) nh = sh - 4.f;
    nx = clampf(nx, 0.f, sw - nw);
    ny = clampf(ny, 0.f, sh - nh);

    g.x = nx;
    g.y = ny;
    g.w = nw;
    g.h = nh;
  }

  g.mouse_was_down = down;
}

// ── 按键 ──────────────────────────────────────────────────
bool draw_key(const char* id, const char* label, ImVec2 size, KeyAnim& st,
              float dt, ImVec4 base, bool enabled) {
  ImGui::PushID(id);
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  const ImGuiID iid = window->GetID("##k");
  const ImVec2 pos = window->DC.CursorPos;
  size.x = std::max(size.x, 8.f);
  size.y = std::max(size.y, 8.f);

  const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
  ImGui::ItemSize(bb);
  if (!ImGui::ItemAdd(bb, iid)) {
    ImGui::PopID();
    return false;
  }

  if (g.interacting()) enabled = false;

  bool hovered = false, held = false, pressed = false;
  if (enabled) pressed = ImGui::ButtonBehavior(bb, iid, &hovered, &held);

  st.hover = lerpf(st.hover, hovered ? 1.f : 0.f, clampf(dt * 16.f, 0.f, 1.f));
  st.press = lerpf(st.press, held ? 1.f : 0.f, clampf(dt * 22.f, 0.f, 1.f));
  if (pressed) st.flash = 1.f;
  st.flash = lerpf(st.flash, 0.f, clampf(dt * 9.f, 0.f, 1.f));

  const float scale = 1.f - st.press * 0.06f + st.flash * 0.02f;
  const ImVec2 c((bb.Min.x + bb.Max.x) * 0.5f, (bb.Min.y + bb.Max.y) * 0.5f);
  const ImVec2 half((bb.Max.x - bb.Min.x) * 0.5f * scale,
                    (bb.Max.y - bb.Min.y) * 0.5f * scale);
  const ImRect db(ImVec2(c.x - half.x, c.y - half.y),
                  ImVec2(c.x + half.x, c.y + half.y));

  ImVec4 col = base;
  col = lerp4(col, th::key_hi, st.hover * 0.55f);
  col = lerp4(col, col * 0.75f, st.press * 0.5f);
  col = lerp4(col, ImVec4(1, 1, 1, col.w), st.flash * 0.2f);
  if (!enabled) col = ImVec4(col.x * 0.5f, col.y * 0.5f, col.z * 0.5f, 0.55f);

  ImDrawList* dl = window->DrawList;
  const float rnd = 7.f;
  dl->AddRectFilled(db.Min, db.Max, col4(col.x, col.y, col.z, col.w), rnd);
  dl->AddRect(db.Min, db.Max, col4(1, 1, 1, 0.07f + st.hover * 0.10f), rnd, 0,
              1.f);

  if (st.flash > 0.04f) {
    float rr = std::min(half.x, half.y) * (0.35f + (1.f - st.flash));
    dl->AddCircleFilled(c, rr, col4(1, 1, 1, 0.12f * st.flash), 18);
  }

  float fs = ImGui::GetFontSize() * 0.90f;
  ImVec2 ts0 = ImGui::CalcTextSize(label);
  if (ts0.x > size.x * 0.88f && ts0.x > 1.f)
    fs *= (size.x * 0.88f) / ts0.x;
  fs = clampf(fs, ImGui::GetFontSize() * 0.50f, ImGui::GetFontSize() * 0.95f);
  const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.f, label);
  dl->AddText(ImGui::GetFont(), fs,
              ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
              col4(th::text.x, th::text.y, th::text.z, enabled ? 0.96f : 0.4f),
              label);

  ImGui::PopID();
  return pressed;
}

/** 读取系统主剪贴板文本（root 下 cmd / service） */
bool fetch_system_clipboard(char* out, size_t cap) {
  if (!out || cap < 2) return false;
  out[0] = 0;
  // 1) Android 10+ cmd clipboard
  FILE* p = popen("cmd clipboard get-primary-clip 2>/dev/null", "r");
  if (p) {
    size_t n = std::fread(out, 1, cap - 1, p);
    pclose(p);
    out[n] = 0;
    // 去掉尾部换行
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    // cmd 可能输出 "ClipData ..." 包装，尽量抽纯文本
    if (n > 0) {
      // 若含 text/plain 取后面
      char* t = std::strstr(out, "T:");
      if (t && t[2]) {
        // 部分机型格式 T:'text'
        char* q = t + 2;
        if (*q == '\'') q++;
        size_t i = 0;
        while (q[i] && q[i] != '\'' && i + 1 < cap) {
          out[i] = q[i];
          i++;
        }
        out[i] = 0;
        n = i;
      }
      if (out[0]) return true;
    }
  }
  // 2) service call clipboard（兼容旧版，输出为 Parcel hex，尽力解析 UTF-16）
  p = popen("service call clipboard 2 i32 1 2>/dev/null | head -c 800", "r");
  if (p) {
    char raw[900]{};
    size_t n = std::fread(raw, 1, sizeof(raw) - 1, p);
    pclose(p);
    raw[n] = 0;
    // 粗提 Parcel 中的可打印 ASCII
    size_t w = 0;
    for (size_t i = 0; i < n && w + 1 < cap; ++i) {
      unsigned char c = (unsigned char)raw[i];
      if (c >= 32 && c < 127) out[w++] = (char)c;
    }
    out[w] = 0;
    // 过滤过短噪音
    if (w > 2 && std::strstr(out, "Result") == nullptr) return true;
    // 尝试从 '...' 提取
    char* a = std::strchr(raw, '\'');
    if (a) {
      char* b = std::strchr(a + 1, '\'');
      if (b && b > a + 1) {
        size_t len = (size_t)(b - a - 1);
        if (len >= cap) len = cap - 1;
        std::memcpy(out, a + 1, len);
        out[len] = 0;
        if (len > 0) return true;
      }
    }
  }
  // 3) Termux 风格：读用户剪贴板文件（若有）
  const char* paths[] = {
      "/data/local/tmp/clip.txt",
      "/sdcard/clip.txt",
      nullptr,
  };
  for (int i = 0; paths[i]; ++i) {
    FILE* f = std::fopen(paths[i], "r");
    if (!f) continue;
    size_t n = std::fread(out, 1, cap - 1, f);
    std::fclose(f);
    out[n] = 0;
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    if (n > 0) return true;
  }
  return false;
}

void push_clip_hist(const char* s) {
  if (!s || !s[0]) return;
  // 去重：与最近一条相同则跳过
  if (g.clip_hist_n > 0 &&
      std::strcmp(g.clip_hist[(g.clip_hist_n - 1) % 12], s) == 0)
    return;
  int idx = g.clip_hist_n % 12;
  std::snprintf(g.clip_hist[idx], sizeof(g.clip_hist[idx]), "%s", s);
  g.clip_hist_n++;
  g.clip_hist_i = g.clip_hist_n - 1;
}

void paste_from_system_clip() {
  char buf[512]{};
  if (fetch_system_clipboard(buf, sizeof(buf))) {
    std::snprintf(g.clip_buf, sizeof(g.clip_buf), "%s", buf);
    push_clip_hist(buf);
    append_text(buf);
    std::snprintf(g.clip_status, sizeof(g.clip_status), "已粘贴系统剪贴板");
  } else {
    std::snprintf(g.clip_status, sizeof(g.clip_status),
                  "剪贴板为空或不可读");
  }
}

void paste_from_hist_cycle() {
  if (g.clip_hist_n <= 0) {
    // 先尝试读系统
    paste_from_system_clip();
    return;
  }
  int idx = g.clip_hist_i % (g.clip_hist_n < 12 ? g.clip_hist_n : 12);
  if (idx < 0) idx = 0;
  append_text(g.clip_hist[idx]);
  std::snprintf(g.clip_status, sizeof(g.clip_status), "历史 #%d", idx + 1);
  // 下次再按切下一条
  g.clip_hist_i = (g.clip_hist_i - 1);
  if (g.clip_hist_i < 0) {
    int n = g.clip_hist_n < 12 ? g.clip_hist_n : 12;
    g.clip_hist_i = n - 1;
  }
}

void handle_key(const KeyDef& k) {
  switch (k.kind) {
    case 1:
      backspace();
      break;
    case 2:
      clear_all();
      break;
    case 3:
      append_text(" ");
      break;
    case 4:
      sync_target_from_draft();
      g.confirmed = true;
      g.want_open = false;
      break;
    case 5:
      g.cancelled = true;
      g.want_open = false;
      break;
    case 6:
      g.shift = !g.shift;
      break;
    case 7:
      paste_from_system_clip();
      break;
    case 8:
      paste_from_hist_cycle();
      break;
    default: {
      const char* ins = k.insert ? k.insert : k.label;
      if (!ins) break;
      if (g.mode == Mode::Abc && !g.shift && ins[0] && !ins[1] &&
          ins[0] >= 'A' && ins[0] <= 'Z') {
        char tmp[2] = {(char)(ins[0] - 'A' + 'a'), 0};
        append_text(tmp);
      } else {
        append_text(ins);
      }
      break;
    }
  }
}

void build_rows(std::vector<std::vector<KeyDef>>& rows) {
  rows.clear();
  if (g.mode == Mode::Hex) {
    rows.push_back({{"1"}, {"2"}, {"3"}, {"A"}, {"B"}, {"C"}});
    rows.push_back({{"4"}, {"5"}, {"6"}, {"D"}, {"E"}, {"F"}});
    rows.push_back({{"7"}, {"8"}, {"9"}, {"0"}, {"0x", "0x", 1.15f},
                    {"#", "#", 0.9f}});
    rows.push_back({{"剪贴", nullptr, 1.1f, 7},
                    {"历史", nullptr, 1.0f, 8},
                    {"清空", nullptr, 1.0f, 2},
                    {"删除", nullptr, 1.15f, 1},
                    {"完成", nullptr, 1.5f, 4}});
  } else if (g.mode == Mode::Abc) {
    rows.push_back({{"Q"}, {"W"}, {"E"}, {"R"}, {"T"}, {"Y"}, {"U"}, {"I"},
                    {"O"}, {"P"}});
    rows.push_back({{"A"}, {"S"}, {"D"}, {"F"}, {"G"}, {"H"}, {"J"}, {"K"},
                    {"L"}});
    rows.push_back({{"Aa", nullptr, 1.2f, 6},
                    {"Z"},
                    {"X"},
                    {"C"},
                    {"V"},
                    {"B"},
                    {"N"},
                    {"M"},
                    {"删除", nullptr, 1.3f, 1}});
    rows.push_back({{"剪贴", nullptr, 1.05f, 7},
                    {"历史", nullptr, 0.95f, 8},
                    {"空格", nullptr, 2.0f, 3},
                    {"删除", nullptr, 1.1f, 1},
                    {"完成", nullptr, 1.3f, 4}});
  } else {
    if (g.asm_page == AsmPage::Mnem) {
      rows.push_back({{"MOV"}, {"LDR"}, {"STR"}, {"ADD"}, {"SUB"}, {"CMP"}});
      rows.push_back({{"B"}, {"BL"}, {"BX"}, {"BLX"}, {"NOP"}, {"RET"}});
      rows.push_back({{"PUSH"}, {"POP"}, {"AND"}, {"ORR"}, {"EOR"}, {"LSL"}});
      rows.push_back({{"LSR"}, {"ASR"}, {"SVC"}, {"CBZ"}, {"MRS"}, {"MSR"}});
    } else if (g.asm_page == AsmPage::Reg) {
      rows.push_back({{"R0"}, {"R1"}, {"R2"}, {"R3"}, {"R4"}, {"R5"}});
      rows.push_back({{"R6"}, {"R7"}, {"R8"}, {"R9"}, {"R10"}, {"R11"}});
      rows.push_back({{"R12"}, {"SP"}, {"LR"}, {"PC"}, {"FP"}, {"XZR"}});
      rows.push_back({{"X0"}, {"X1"}, {"X2"}, {"W0"}, {"W1"}, {"W2"}});
    } else {
      rows.push_back({{"#"}, {","}, {"["}, {"]"}, {"{"}, {"}"}});
      rows.push_back({{"="}, {";"}, {"+"}, {"-"}, {"!"}, {":"}});
      rows.push_back({{"0x", "0x"}, {"//", "//"}, {".", "."}, {"_", "_"},
                      {"\\n", "\n"}, {"\"", "\""}});
    }
    rows.push_back({{"剪贴", nullptr, 1.0f, 7},
                    {"历史", nullptr, 0.95f, 8},
                    {"空格", nullptr, 1.3f, 3},
                    {"删除", nullptr, 1.15f, 1},
                    {"完成", nullptr, 1.35f, 4}});
  }
}

struct Metrics {
  float pad = 8.f;
  float gap = 3.f;
  float preview_h = 34.f;
  float mode_h = 28.f;
  float asm_h = 0.f;
  float bottom = 10.f;
};

Metrics compute_metrics(float pw, float ph, int rows, bool has_asm) {
  Metrics m;
  m.pad = clampf(pw * 0.02f, 6.f, 10.f);
  m.gap = clampf(pw * 0.007f, 2.5f, 4.5f);
  const float hs = clampf(ph / 260.f, 0.8f, 1.8f);
  m.preview_h = clampf(34.f * hs, 28.f, 48.f);
  m.mode_h = clampf(28.f * hs, 24.f, 36.f);
  m.asm_h = has_asm ? clampf(26.f * hs, 22.f, 34.f) : 0.f;
  m.bottom = 12.f;  // 缩放把手
  (void)rows;
  return m;
}

void draw_segmented(ImDrawList* dl, ImVec2 origin, float width, float height,
                    const char* const* labels, int count, int selected,
                    float& slide, float dt, float alpha, const char* bar_id,
                    void (*on_pick)(int, void*)) {
  float target = (float)selected;
  slide = lerpf(slide, target, clampf(dt * 14.f, 0.f, 1.f));
  const float gap = 2.f;
  const float cell = (width - gap * (float)(count - 1)) / (float)count;
  const float rnd = 8.f;

  dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height),
                    col4(0.07f, 0.08f, 0.10f, 0.95f * alpha), rnd);

  float sx = origin.x + slide * (cell + gap);
  dl->AddRectFilled(ImVec2(sx + 1, origin.y + 1),
                    ImVec2(sx + cell - 1, origin.y + height - 1),
                    col4(th::accent_dim.x, th::accent_dim.y, th::accent_dim.z,
                         0.95f * alpha),
                    rnd - 2.f);

  ImGui::PushID(bar_id ? bar_id : "seg");
  ImGui::SetCursorScreenPos(origin);
  for (int i = 0; i < count; ++i) {
    if (i) ImGui::SameLine(0, gap);
    ImGui::PushID(i);
    char bid[24];
    std::snprintf(bid, sizeof(bid), "##s%d", i);
    ImGui::InvisibleButton(bid, ImVec2(cell, height));
    if (ImGui::IsItemClicked() && on_pick && !g.interacting()) on_pick(i, nullptr);
    ImVec2 ts = ImGui::CalcTextSize(labels[i]);
    float cx = origin.x + i * (cell + gap) + (cell - ts.x) * 0.5f;
    float cy = origin.y + (height - ts.y) * 0.5f;
    bool sel = (i == selected);
    dl->AddText(ImVec2(cx, cy),
                col4(sel ? 0.96f : th::muted.x, sel ? 0.98f : th::muted.y,
                     sel ? 1.f : th::muted.z, alpha),
                labels[i]);
    ImGui::PopID();
  }
  ImGui::PopID();
}

void draw_keyboard_body(float dt, float alpha) {
  ensure_default_geom();

  std::vector<std::vector<KeyDef>> rows;
  build_rows(rows);
  const bool has_asm = (g.mode == Mode::Asm);
  Metrics M = compute_metrics(g.w, g.h, (int)rows.size(), has_asm);

  // 打开动画：仅淡入 + 轻微上浮；拖动中关闭上浮，避免坐标错位
  const float t = ease_out_cubic(g.anim);
  const float y_nudge =
      (g.interacting() || g.anim > 0.98f) ? 0.f : (1.f - t) * 16.f;

  const float wx = g.x;
  const float wy = g.y + y_nudge;

  ImGui::SetNextWindowPos(ImVec2(wx, wy), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(g.w, g.h), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.f);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));

  ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav;

  if (ImGui::Begin("##soft_ime", nullptr, flags)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp(wx, wy);
    const ImVec2 ws(g.w, g.h);
    const float rnd = 14.f;
    const float pad = M.pad;
    const float content_w = ws.x - pad * 2.f;

    // 阴影 + 面板
    dl->AddRectFilled(ImVec2(wp.x + 2, wp.y + 4),
                      ImVec2(wp.x + ws.x + 2, wp.y + ws.y + 4),
                      col4(0, 0, 0, 0.30f * alpha), rnd);
    dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y),
                      col4(th::panel.x, th::panel.y, th::panel.z,
                           th::panel.w * alpha),
                      rnd);

    // ── 标题栏（整条可拖，视觉独立）──
    {
      ImVec2 t0 = wp;
      ImVec2 t1(wp.x + ws.x, wp.y + kTitleH);
      dl->AddRectFilled(t0, t1,
                        col4(th::title_bar.x, th::title_bar.y, th::title_bar.z,
                             alpha),
                        rnd, ImDrawFlags_RoundCornersTop);
      // 顶部分割线
      dl->AddRectFilled(ImVec2(t0.x, t1.y - 1.f), t1,
                        col4(th::accent.x, th::accent.y, th::accent.z,
                             0.35f * alpha));

      // 拖动手柄
      float hx = wp.x + ws.x * 0.5f;
      float hy = wp.y + 7.f;
      bool drag_hot = g.dragging || g.drag_pending;
      dl->AddRectFilled(ImVec2(hx - 18.f, hy), ImVec2(hx + 18.f, hy + 3.5f),
                        col4(1, 1, 1, (drag_hot ? 0.45f : 0.22f) * alpha), 2.f);

      // 标题
      dl->AddText(ImVec2(wp.x + pad, wp.y + (kTitleH - ImGui::GetFontSize()) * 0.5f + 2.f),
                  col4(th::text.x, th::text.y, th::text.z, alpha), g.title);

      // 尺寸
      char sz[24];
      std::snprintf(sz, sizeof(sz), "%.0fx%.0f", g.w, g.h);
      ImVec2 szs = ImGui::CalcTextSize(sz);
      dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.72f,
                  ImVec2(wp.x + ws.x - pad - 30.f - szs.x * 0.72f,
                         wp.y + 11.f),
                  col4(th::muted.x, th::muted.y, th::muted.z, 0.75f * alpha),
                  sz);

      // 关闭
      ImVec2 cp(wp.x + ws.x - pad - 22.f, wp.y + 7.f);
      ImGui::SetCursorScreenPos(cp);
      ImGui::InvisibleButton("##ime_close", ImVec2(22.f, 22.f));
      bool xh = ImGui::IsItemHovered();
      if (ImGui::IsItemClicked() && !g.interacting()) {
        g.cancelled = true;
        g.want_open = false;
      }
      ImVec2 xc(cp.x + 11.f, cp.y + 11.f);
      dl->AddCircleFilled(xc, 10.f,
                          col4(0.22f, 0.18f, 0.20f, (xh ? 1.f : 0.8f) * alpha),
                          16);
      dl->AddLine(ImVec2(xc.x - 4, xc.y - 4), ImVec2(xc.x + 4, xc.y + 4),
                  col4(0.95f, 0.9f, 0.9f, alpha), 1.6f);
      dl->AddLine(ImVec2(xc.x + 4, xc.y - 4), ImVec2(xc.x - 4, xc.y + 4),
                  col4(0.95f, 0.9f, 0.9f, alpha), 1.6f);
    }

    dl->AddRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y),
                col4(th::panel_edge.x, th::panel_edge.y, th::panel_edge.z,
                     th::panel_edge.w * alpha),
                rnd, 0, 1.1f);

    float cy = wp.y + kTitleH + 6.f;
    float cx0 = wp.x + pad;

    // ── 预览 ──
    {
      ImVec2 p0(cx0, cy);
      ImVec2 p1(cx0 + content_w, cy + M.preview_h);
      dl->AddRectFilled(p0, p1,
                        col4(th::preview_bg.x, th::preview_bg.y,
                             th::preview_bg.z, alpha),
                        8.f);
      dl->AddRect(p0, p1,
                  col4(th::accent.x, th::accent.y, th::accent.z, 0.30f * alpha),
                  8.f, 0, 1.f);

      const char* draw_s = g.draft;
      float max_w = content_w - 18.f;
      if (g.draft[0] && ImGui::CalcTextSize(draw_s).x > max_w) {
        size_t start = 0;
        while (start < g.draft_len &&
               ImGui::CalcTextSize(g.draft + start).x > max_w)
          start++;
        draw_s = g.draft + start;
      }
      float ty = p0.y + (M.preview_h - ImGui::GetFontSize()) * 0.5f;
      if (!g.draft[0]) {
        dl->AddText(ImVec2(p0.x + 10.f, ty),
                    col4(th::muted.x, th::muted.y, th::muted.z, 0.6f * alpha),
                    "点按输入 · 剪贴=系统剪贴板");
      } else {
        dl->AddText(ImVec2(p0.x + 10.f, ty),
                    col4(th::text.x, th::text.y, th::text.z, alpha), draw_s);
        float blink = (std::sin(g.time * 5.5f) > 0.f) ? 0.9f : 0.12f;
        ImVec2 cts = ImGui::CalcTextSize(draw_s);
        float cxs = p0.x + 10.f + cts.x + 1.f;
        dl->AddRectFilled(ImVec2(cxs, ty + 1.f),
                          ImVec2(cxs + 1.6f, ty + ImGui::GetFontSize() - 1.f),
                          col4(th::accent.x, th::accent.y, th::accent.z,
                               blink * alpha));
      }
      if (g.clip_status[0]) {
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.72f,
                    ImVec2(p0.x + 10.f, p1.y - ImGui::GetFontSize() * 0.85f),
                    col4(th::accent.x, th::accent.y, th::accent.z, 0.85f * alpha),
                    g.clip_status);
      }
      cy = p1.y + 6.f;
    }

    // ── 模式 ──
    {
      static const char* kModes[] = {"HEX", "ABC", "ASM"};
      ImVec2 origin(cx0, cy);
      auto pick = [](int i, void*) { g.mode = (Mode)i; };
      draw_segmented(dl, origin, content_w, M.mode_h, kModes, 3, (int)g.mode,
                     g.mode_slide, dt, alpha, "mode_bar", pick);
      cy = origin.y + M.mode_h + 5.f;
    }

    if (has_asm) {
      static const char* kAsm[] = {"指令", "寄存器", "符号"};
      ImVec2 origin(cx0, cy);
      auto pick = [](int i, void*) { g.asm_page = (AsmPage)i; };
      draw_segmented(dl, origin, content_w, M.asm_h, kAsm, 3, (int)g.asm_page,
                     g.asm_slide, dt, alpha, "asm_bar", pick);
      cy = origin.y + M.asm_h + 5.f;
    }

    // ── 按键区：占满剩余高度 ──
    {
      float key_bottom = wp.y + ws.y - M.bottom - pad * 0.3f;
      float avail = key_bottom - cy;
      int nr = (int)rows.size();
      float gap = M.gap;
      float key_h = 28.f;
      if (nr > 0 && avail > 1.f)
        key_h = std::max(16.f, avail / (float)nr - gap);

      for (int ri = 0; ri < nr; ++ri) {
        auto& row = rows[ri];
        float units = 0.f;
        for (auto& k : row) units += k.width;
        if (units < 0.01f) units = 1.f;
        float unit_w = (content_w - gap * (float)(row.size() - 1)) / units;

        float row_w = 0.f;
        for (auto& k : row) row_w += unit_w * k.width;
        row_w += gap * (float)(row.size() - 1);
        float indent = std::max(0.f, (content_w - row_w) * 0.5f);

        float row_y = cy + (float)ri * (key_h + gap);
        float x = cx0 + indent;

        // 底行右侧为缩放区：完成键略缩，避免误触
        for (int ci = 0; ci < (int)row.size(); ++ci) {
          auto& k = row[ci];
          float kw = unit_w * k.width;
          // 最后一行最后一键与右下角把手错开：最后一列略收窄
          if (ri == nr - 1 && ci == (int)row.size() - 1)
            kw = std::max(kw - 6.f, 28.f);

          ImGui::SetCursorScreenPos(ImVec2(x, row_y));
          ImVec4 col = th::key;
          if (k.kind == 1) col = th::del;
          else if (k.kind == 2 || k.kind == 3) col = th::key_fn;
          else if (k.kind == 4) col = th::ok;
          else if (k.kind == 6) col = g.shift ? th::accent_dim : th::key_fn;
          else if (k.kind == 7 || k.kind == 8) col = th::accent_dim;

          char label_buf[8];
          const char* lab = k.label;
          if (g.mode == Mode::Abc && k.kind == 0 && k.label[0] && !k.label[1] &&
              k.label[0] >= 'A' && k.label[0] <= 'Z' && !g.shift) {
            label_buf[0] = (char)(k.label[0] - 'A' + 'a');
            label_buf[1] = 0;
            lab = label_buf;
          }

          char id[24];
          std::snprintf(id, sizeof(id), "k%d_%d", ri, ci);
          KeyAnim& ka = g.keys[(g.key_n++) % 128];
          if (draw_key(id, lab, ImVec2(kw, key_h), ka, dt, col, true))
            handle_key(k);
          x += unit_w * k.width + gap;
        }
      }
    }

    // ── 右下角缩放把手（更大更明显）──
    {
      ImGuiIO& io = ImGui::GetIO();
      int hov = g.resizing != R_None
                    ? g.resizing
                    : hit_resize(io.MousePos, g.x, g.y, g.w, g.h);
      bool hot = (hov & (R_R | R_B)) == (R_R | R_B) ||
                 (g.resizing & (R_R | R_B)) == (R_R | R_B);
      const float s = 32.f;
      ImVec2 a(wp.x + ws.x - s - 3.f, wp.y + ws.y - s - 3.f);
      ImVec2 b(wp.x + ws.x - 3.f, wp.y + ws.y - 3.f);
      dl->AddRectFilled(a, b,
                        col4(th::accent_dim.x, th::accent_dim.y, th::accent_dim.z,
                             (hot ? 1.f : 0.65f) * alpha),
                        7.f);
      dl->AddRect(a, b,
                  col4(th::accent.x, th::accent.y, th::accent.z,
                       (hot ? 1.f : 0.45f) * alpha),
                  7.f, 0, 1.3f);
      for (int i = 0; i < 3; ++i) {
        float o = 8.f + i * 5.5f;
        dl->AddLine(ImVec2(b.x - o, b.y - 6.f), ImVec2(b.x - 6.f, b.y - o),
                    col4(1, 1, 1, (0.5f + i * 0.12f) * alpha), 2.f);
      }
    }

    // 拖动中：边框高亮
    if (g.dragging) {
      dl->AddRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y),
                  col4(th::accent.x, th::accent.y, th::accent.z, 0.7f * alpha),
                  rnd, 0, 2.f);
    } else if (g.resizing != R_None) {
      dl->AddRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y),
                  col4(0.4f, 0.9f, 0.6f, 0.7f * alpha), rnd, 0, 2.f);
    }
  }
  ImGui::End();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(3);
}

}  // namespace

bool open(char* buf, size_t cap, const char* title, Mode mode) {
  if (!buf || cap < 2) return false;
  g.target = buf;
  g.cap = cap;
  g.mode = mode;
  g.asm_page = AsmPage::Mnem;
  g.shift = true;
  g.confirmed = false;
  g.cancelled = false;
  g.dragging = false;
  g.drag_pending = false;
  g.resizing = R_None;
  g.mouse_was_down = false;
  if (title && title[0])
    std::snprintf(g.title, sizeof(g.title), "%s", title);
  else
    std::snprintf(g.title, sizeof(g.title), "%s", "输入");
  load_draft_from_target();
  ensure_default_geom();
  g.want_open = true;
  g.open = true;
  return true;
}

void close() {
  g.want_open = false;
  g.dragging = false;
  g.drag_pending = false;
  g.resizing = R_None;
  if (g.anim < 0.05f) {
    g.open = false;
    g.target = nullptr;
  }
}

bool is_open() { return g.open || g.want_open || g.anim > 0.01f; }

bool just_confirmed() {
  bool v = g.confirmed;
  g.confirmed = false;
  return v;
}

bool just_cancelled() {
  bool v = g.cancelled;
  g.cancelled = false;
  return v;
}

void update_and_draw(float dt) {
  g.time += dt;
  g.key_n = 0;

  const float target = g.want_open ? 1.f : 0.f;
  g.anim = lerpf(g.anim, target, clampf(dt * 12.f, 0.f, 1.f));
  if (std::fabs(g.anim - target) < 0.002f) g.anim = target;

  if (!g.want_open && g.anim < 0.01f) {
    g.open = false;
    g.anim = 0.f;
    g.target = nullptr;
    g.dragging = false;
    g.drag_pending = false;
    g.resizing = R_None;
    g.mouse_was_down = false;
    return;
  }

  g.open = true;
  const float alpha = ease_out_cubic(g.anim);

  // ★ 先处理指针，再画窗口，避免拖动滞后一帧
  process_pointer();

  auto disp = vkeng::display();
  // 遮罩：拖缩时不可关闭
  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2((float)disp.width, (float)disp.height),
                           ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  if (ImGui::Begin("##soft_ime_dim", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                       ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoNav |
                       ImGuiWindowFlags_NoBringToFrontOnFocus)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, 0),
                      ImVec2((float)disp.width, (float)disp.height),
                      col4(0.02f, 0.03f, 0.05f, 0.30f * alpha));
    ImGui::InvisibleButton("##dim_hit",
                           ImVec2((float)disp.width, (float)disp.height));
    if (ImGui::IsItemClicked() && !g.interacting()) {
      g.cancelled = true;
      g.want_open = false;
    }
  }
  ImGui::End();
  ImGui::PopStyleVar();

  draw_keyboard_body(dt, alpha);
}

}  // namespace soft_ime
