#include "float_app.hpp"

#include "touch.hpp"
#include "vk_engine.hpp"
#include "soft_ime.hpp"
#include "mem_core.hpp"
#include "mem_disasm.hpp"
#include "mem_bp.hpp"
#include "mem_table.hpp"
#include "mem_ptrscan.hpp"
#include "mem_struct.hpp"
#include "mem_game.hpp"
#include "mem_icon.hpp"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <vector>

namespace float_app {
namespace {

// ── 动画工具 ──────────────────────────────────────────────
inline float clampf(float v, float a, float b) {
  return v < a ? a : (v > b ? b : v);
}
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float ease_out_cubic(float t) {
  t = clampf(t, 0.f, 1.f);
  float u = 1.f - t;
  return 1.f - u * u * u;
}
inline float ease_out_back(float t) {
  t = clampf(t, 0.f, 1.f);
  const float c1 = 1.70158f;
  const float c3 = c1 + 1.f;
  return 1.f + c3 * std::pow(t - 1.f, 3.f) + c1 * std::pow(t - 1.f, 2.f);
}
[[maybe_unused]] inline float ease_in_out(float t) {
  t = clampf(t, 0.f, 1.f);
  return t < 0.5f ? 2.f * t * t : 1.f - std::pow(-2.f * t + 2.f, 2.f) / 2.f;
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

// ── 卡哇伊宝宝主题（粉彩奶油）────────────────────────────
namespace theme {
// 奶油粉底 + 糖果色（全部浅色底，保证深色字永远可见）
constexpr ImVec4 bg_deep(1.00f, 0.96f, 0.97f, 0.97f);     // 奶油白粉
[[maybe_unused]] constexpr ImVec4 bg_card(1.00f, 0.92f, 0.94f, 0.95f);
constexpr ImVec4 accent(1.00f, 0.62f, 0.76f, 1.f);        // 草莓粉（略亮）
constexpr ImVec4 accent2(0.82f, 0.74f, 0.98f, 1.f);       // 棉花糖紫
constexpr ImVec4 success(0.62f, 0.90f, 0.76f, 1.f);       // 薄荷绿
constexpr ImVec4 warn(1.00f, 0.82f, 0.62f, 1.f);          // 蜜桃橙
constexpr ImVec4 danger(1.00f, 0.62f, 0.70f, 1.f);        // 珊瑚红
constexpr ImVec4 text(0.28f, 0.18f, 0.24f, 1.f);          // 深可可字（高对比）
constexpr ImVec4 text_on_dark(1.00f, 0.98f, 0.99f, 1.f); // 浅底反差白字
constexpr ImVec4 muted(0.55f, 0.42f, 0.48f, 1.f);         // 豆沙灰粉（加深可读）
constexpr ImVec4 tab_idle(1.00f, 0.90f, 0.92f, 1.f);      // 未选中 tab
constexpr ImVec4 bubble(1.00f, 0.78f, 0.86f, 1.f);        // 气泡粉
constexpr ImVec4 idle(1.00f, 0.90f, 0.93f, 1.f);          // 默认未选中按钮
constexpr ImVec4 secondary(0.88f, 0.90f, 0.98f, 1.f);     // 浅蓝紫次要
constexpr ImVec4 teal(0.72f, 0.92f, 0.88f, 1.f);          // 浅青绿
constexpr ImVec4 peach(1.00f, 0.88f, 0.78f, 1.f);         // 浅蜜桃
constexpr float btn_min_h = 48.f;                          // 触控最小高度
constexpr float chip_h = 40.f;                             // 行内小按钮高度
constexpr float btn_round = 18.f;                          // 大圆角
}  // namespace theme

// 亮度：>0.55 用深字，否则用白字（避免「按钮看不见字」）
inline float luminance(ImVec4 c) {
  return 0.299f * c.x + 0.587f * c.y + 0.114f * c.z;
}
inline ImU32 contrast_text_u32(ImVec4 bg, float a = 0.98f) {
  if (luminance(bg) > 0.55f)
    return col4(theme::text.x, theme::text.y, theme::text.z, a);
  return col4(theme::text_on_dark.x, theme::text_on_dark.y,
              theme::text_on_dark.z, a);
}

// UTF-8 安全截断到最多 max_chars 个字符，末尾加 …
void utf8_clip(const char* src, char* dst, size_t dst_cap, int max_chars) {
  if (!src || !dst || dst_cap < 4) {
    if (dst && dst_cap) dst[0] = 0;
    return;
  }
  int chars = 0;
  size_t i = 0;
  while (src[i] && chars < max_chars) {
    unsigned char c = (unsigned char)src[i];
    size_t n = 1;
    if (c >= 0xF0)
      n = 4;
    else if (c >= 0xE0)
      n = 3;
    else if (c >= 0xC0)
      n = 2;
    if (i + n + 4 >= dst_cap) break;
    for (size_t k = 0; k < n && src[i + k]; ++k) dst[i + k] = src[i + k];
    i += n;
    chars++;
  }
  if (src[i]) {
    // 省略号
    if (i + 4 < dst_cap) {
      dst[i++] = (char)0xE2;
      dst[i++] = (char)0x80;
      dst[i++] = (char)0xA6;  // …
    }
  }
  dst[i] = 0;
}

// ── 动画按钮（大触控 + 软萌外观 + 自动对比字色）──────────
struct AnimBtn {
  float hover = 0.f;
  float press = 0.f;
  float flash = 0.f;
};

bool anim_button(const char* id, const char* label, ImVec2 size, AnimBtn& st,
                 float dt, ImVec4 base_col, bool enabled = true) {
  ImGui::PushID(id);
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  const ImGuiID iid = window->GetID("##ab");
  const ImVec2 pos = window->DC.CursorPos;
  if (size.x <= 0.f)
    size.x = ImGui::GetContentRegionAvail().x;
  if (size.y <= 0.f)
    size.y = std::max(ImGui::GetFrameHeight() * 1.55f, theme::btn_min_h);
  else if (size.y < theme::chip_h)
    size.y = theme::chip_h;  // 行内按钮也保证可读高度

  const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
  ImGui::ItemSize(bb);
  if (!ImGui::ItemAdd(bb, iid)) {
    ImGui::PopID();
    return false;
  }

  bool hovered = false, held = false;
  bool pressed = false;
  if (enabled)
    pressed = ImGui::ButtonBehavior(bb, iid, &hovered, &held);
  else {
    ImGui::ButtonBehavior(bb, iid, &hovered, &held, ImGuiButtonFlags_None);
    hovered = held = pressed = false;
  }

  const float speed = 12.f;
  st.hover = lerpf(st.hover, hovered ? 1.f : 0.f, clampf(dt * speed, 0.f, 1.f));
  st.press = lerpf(st.press, held ? 1.f : 0.f, clampf(dt * 18.f, 0.f, 1.f));
  if (pressed) st.flash = 1.f;
  st.flash = lerpf(st.flash, 0.f, clampf(dt * 6.f, 0.f, 1.f));

  // 软弹：轻微放大，像果冻
  const float scale =
      1.f + st.hover * 0.04f - st.press * 0.06f + st.flash * 0.03f;
  const ImVec2 center((bb.Min.x + bb.Max.x) * 0.5f,
                      (bb.Min.y + bb.Max.y) * 0.5f);
  const ImVec2 half((bb.Max.x - bb.Min.x) * 0.5f * scale,
                    (bb.Max.y - bb.Min.y) * 0.5f * scale);
  const ImRect draw_bb(ImVec2(center.x - half.x, center.y - half.y),
                       ImVec2(center.x + half.x, center.y + half.y));

  ImVec4 col = base_col;
  // 强制浅色：若误传深色底，抬亮到可读粉彩
  if (luminance(col) < 0.45f) {
    col = ImVec4(col.x * 0.35f + 0.65f, col.y * 0.35f + 0.62f,
                 col.z * 0.35f + 0.68f, col.w);
  }
  col = lerp4(col, col + ImVec4(0.06f, 0.04f, 0.05f, 0.f), st.hover);
  col = lerp4(col, col * 0.92f, st.press * 0.4f);
  col = lerp4(col, ImVec4(1.f, 0.96f, 0.98f, col.w), st.flash * 0.3f);
  if (!enabled)
    col = ImVec4(col.x * 0.75f + 0.2f, col.y * 0.75f + 0.2f,
                 col.z * 0.75f + 0.2f, 0.55f);

  ImDrawList* dl = window->DrawList;
  const float rounding = std::min(theme::btn_round, half.y);
  // 软阴影（粉色调）
  dl->AddRectFilled(ImVec2(draw_bb.Min.x + 2, draw_bb.Min.y + 3),
                    ImVec2(draw_bb.Max.x + 2, draw_bb.Max.y + 3),
                    col4(0.85f, 0.55f, 0.65f, 0.18f + st.hover * 0.1f),
                    rounding);
  dl->AddRectFilled(draw_bb.Min, draw_bb.Max,
                    col4(col.x, col.y, col.z, col.w), rounding);
  // 顶部奶油高光（减弱，避免冲掉字）
  dl->AddRectFilledMultiColor(
      draw_bb.Min, ImVec2(draw_bb.Max.x, draw_bb.Min.y + half.y * 0.55f),
      col4(1, 1, 1, 0.28f + st.hover * 0.08f),
      col4(1, 1, 1, 0.28f + st.hover * 0.08f), col4(1, 1, 1, 0.f),
      col4(1, 1, 1, 0.f));
  dl->AddRect(draw_bb.Min, draw_bb.Max,
              col4(1.f, 0.75f, 0.85f, 0.65f + st.hover * 0.2f), rounding, 0,
              1.8f);

  if (st.flash > 0.02f) {
    const float rr = half.x * (0.2f + (1.f - st.flash) * 0.9f);
    dl->AddCircleFilled(center, rr, col4(1, 0.92f, 0.96f, 0.22f * st.flash),
                        28);
  }

  // 对比色文字；UTF-8 安全截断；限制最小字号
  float fs = ImGui::GetFontSize();
  if (fs < 16.f) fs = 16.f;  // 触屏最低可读
  ImVec2 ts0 = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.f, label);
  const float max_w = size.x - 10.f;
  if (ts0.x > max_w && ts0.x > 1.f) {
    float scale_fs = max_w / ts0.x;
    fs *= std::max(scale_fs, 0.78f);
  }
  const char* draw_label = label;
  char clip[64];
  ImVec2 ts2 = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.f, label);
  if (ts2.x > max_w) {
    // 按字符数逐步缩短
    for (int mc = 8; mc >= 2; --mc) {
      utf8_clip(label, clip, sizeof(clip), mc);
      ts2 = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.f, clip);
      if (ts2.x <= max_w) break;
    }
    draw_label = clip;
  }
  const ImVec2 ts3 =
      ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.f, draw_label);
  // 轻微描边，粉底上更清晰
  ImU32 tc = contrast_text_u32(col, enabled ? 1.f : 0.5f);
  ImVec2 tp(center.x - ts3.x * 0.5f, center.y - ts3.y * 0.5f);
  ImU32 outline = col4(1.f, 0.97f, 0.98f, enabled ? 0.55f : 0.25f);
  for (int ox = -1; ox <= 1; ++ox)
    for (int oy = -1; oy <= 1; ++oy)
      if (ox || oy)
        dl->AddText(ImGui::GetFont(), fs,
                    ImVec2(tp.x + (float)ox, tp.y + (float)oy), outline,
                    draw_label);
  dl->AddText(ImGui::GetFont(), fs, tp, tc, draw_label);

  ImGui::PopID();
  return pressed;
}

/** 行内芯片按钮：替代 SmallButton，保证中文可见、触控够大 */
bool chip_button(const char* label, ImVec4 col = theme::bubble) {
  ImGui::PushStyleColor(ImGuiCol_Button, col);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                        ImVec4(col.x * 0.95f, col.y * 0.92f, col.z * 0.95f, 1.f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                        ImVec4(col.x * 0.9f, col.y * 0.85f, col.z * 0.9f, 1.f));
  ImGui::PushStyleColor(ImGuiCol_Text, theme::text);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.f, 10.f));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.f);
  // 最小高度
  float h = std::max(ImGui::GetFrameHeight(), theme::chip_h);
  bool r = ImGui::Button(label, ImVec2(0.f, h));
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(4);
  return r;
}

// 画小爱心装饰
void draw_heart(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
  // 两圆 + 三角
  dl->AddCircleFilled(ImVec2(c.x - s * 0.35f, c.y - s * 0.15f), s * 0.38f, col,
                      12);
  dl->AddCircleFilled(ImVec2(c.x + s * 0.35f, c.y - s * 0.15f), s * 0.38f, col,
                      12);
  dl->AddTriangleFilled(ImVec2(c.x - s * 0.72f, c.y),
                        ImVec2(c.x + s * 0.72f, c.y),
                        ImVec2(c.x, c.y + s * 0.75f), col);
}

// ── 内存调试 UI 状态（CE 风格分区）────────────────────────
/**
 * 主导航（6 项，CE 工作流）：
 *  进程 → 扫描(搜+结果) → 地址(表+浏览) → 分析(指针+结构)
 *  → 调试(断点/补丁/注入/地图) → 自动(变速/脚本/AA)
 * 子功能用页内芯片切换，避免「工具」一页拉到底。
 */
enum class Tab : int {
  Process = 0,
  Scan,      // 搜索 + 结果
  Addr,      // 地址表 + 内存浏览
  Analyze,   // 指针 + 结构
  Debug,     // 原工具：调试/补丁/注入/地图
  Auto,      // 游戏向自动化
  COUNT
};

static const char* kTabNames[] = {"进程", "扫描", "地址", "分析", "调试", "自动"};

enum class ValueType : int {
  I8 = 0,
  I16,
  I32,
  I64,
  F32,
  F64,
  Hex,
  StrUtf8,
  StrUtf16,
  COUNT
};
static const char* kValueTypeNames[] = {
    "I8", "I16", "I32", "I64", "Float", "Double", "Hex", "UTF8", "U16LE"};

enum class SearchMode : int {
  Exact = 0,
  Greater,
  Less,
  Between,
  Changed,
  Unchanged,
  Increased,
  Decreased,
  Unknown,
  Fuzzy,
  COUNT
};
static const char* kSearchModeNames[] = {
    "精确数值", "大于",   "小于",   "区间",   "已变化",
    "未变化",   "已增加", "已减少", "未知初始", "模糊匹配"};

struct UiResult {
  uintptr_t addr = 0;
  char value[48]{};
  bool frozen = false;
  uint64_t bits = 0;
};

struct Ui {
  bool want_open = false;
  float open_anim = 0.f;
  bool running = true;

  bool ball_pos_init = false;
  ImVec2 ball_pos{80.f, 400.f};
  float ball_r = 52.f;  // 略大更好点
  float ball_press = 0.f;
  float ball_time = 0.f;
  float ball_spin = 0.f;

  // 主面板几何（用户可拖/缩：右/下/角 双向）
  bool panel_geom_init = false;
  ImVec2 panel_pos{0, 0};
  ImVec2 panel_size{0, 0};
  bool panel_dragging = false;
  bool panel_resizing = false;
  int panel_resize_mask = 0;  // bit0=宽 bit1=高
  ImVec2 panel_grab_m{}, panel_grab_pos{}, panel_grab_size{};

  Tab tab = Tab::Process;
  float tab_anim = 1.f;
  Tab tab_prev = Tab::Process;
  // 页内子导航（芯片）
  int scan_sub = 0;     // 0搜索 1结果
  int addr_sub = 0;     // 0地址表 1浏览
  int analyze_sub = 0;  // 0指针 1结构
  int debug_sub = 0;    // 0控制 1断点 2补丁 3注入 4地图
  int auto_sub = 0;     // 0变速 1热键 2脚本 3AA 4Trainer

  // 搜索
  ValueType vtype = ValueType::I32;
  SearchMode smode = SearchMode::Exact;
  mem::RegionFilter region_filter = mem::RegionFilter::Writable;
  char value_buf[128] = "100";  // 数值 / Hex / 字符串
  char value_buf2[64] = "200";  // Between 上限 / Fuzzy 容差 / 字符串 ci
  char result_filter[64] = "";   // 结果列表模糊过滤
  int search_round = 0;
  int result_count = 0;
  bool searching = false;
  float search_spin = 0.f;
  float refresh_acc = 0.f;
  bool str_ignore_case = false;  // 字符串搜索忽略大小写

  // 扫描结果（分页）
  std::vector<UiResult> results;
  int selected_result = -1;
  int result_page = 0;       // 0-based
  int result_page_size = 50; // 每页条数

  // 编辑 / 浏览共用
  char edit_addr[32] = "0x0";
  char edit_value[64] = "0";
  char hex_preview[16][3]{};
  char ascii_preview[17]{};
  // 内存浏览器
  uintptr_t browse_base = 0;
  uint8_t browse_bytes[512]{};
  int browse_rows = 16;  // 16*16=256
  bool browse_valid = false;

  // CE 地址表
  mem::AddressTable addr_table;
  char table_desc[64] = "新地址";
  float table_refresh_t = 0.f;

  // 进程
  char process_filter[64] = "";
  int selected_list = -1;
  std::vector<mem::ProcInfo> procs;
  int attached_pid = -1;
  char attached_name[128]{};
  bool skip_no_icon = true;   // 过滤无应用图标的进程
  bool tencent_only = false;  // 仅腾讯进程
  bool filter_asm_noise = true;  // 过滤无用汇编

  // 动态调试
  mem::Regs dbg_regs{};
  mem::FpRegs dbg_fp{};
  char patch_hex_buf[128] = "D5 03 20 1F";  // NOP
  char patch_asm_buf[192] = "nop";          // 汇编写回
  char dbg_reg_name[16] = "x0";
  char dbg_reg_val[32] = "0";
  char bp_cond_buf[48] = "x0==1";
  char inject_path[192] = "/data/local/tmp/libtest.so";
  char dlopen_addr_buf[32] = "0x0";
  char remote_fn_buf[32] = "0x0";
  char module_filter[64] = "";
  std::vector<mem::ThreadInfo> threads;
  std::vector<mem::ModuleInfo> modules;
  std::vector<uintptr_t> stack_frames;
  std::vector<mem::TraceEntry> trace_log;
  int dbg_tid = 0;
  bool auto_regs_on_hit = true;

  // 游戏向 / 自动化
  float speed_mult = 2.f;
  char script_buf[4096] =
      "-- Lua 5.4  (内嵌)  mem.help() 看 API\n"
      "print(\"MemDbg Lua ready\", mem.is_attached(), mem.pid())\n"
      "\n"
      "-- 写整数 / 浮点\n"
      "-- mem.write_i32(0x12345678, 999)\n"
      "-- mem.write_f32(0x12345678, 99.5)\n"
      "-- print(mem.read_i32(0x12345678))\n"
      "\n"
      "-- 字符串 / hex\n"
      "-- mem.write_str(0x12345678, \"HelloQQ\")\n"
      "-- mem.write_hex(0x12345678, \"48 65 6C 6C 6F 00\")\n"
      "\n"
      "-- AOB 后写入\n"
      "-- local a = mem.aob(\"?? 00 ?? FF\", \"libxxx.so\")\n"
      "-- if a then mem.write_i32(a, 1); mem.freeze(a, \"i32\", 1) end\n"
      "\n"
      "-- 循环\n"
      "-- for i=1,5 do mem.write_i32(0x12345678, i); mem.sleep(200) end\n"
      "\n"
      "-- 变速\n"
      "-- mem.speed(2.0)\n"
      "mem.help()\n";
  char aa_buf[2048] =
      "[ENABLE]\n"
      "aobscan(sc, 1F 20 03 D5)\n"
      "nop(sc, 1)\n"
      "[DISABLE]\n";
  char hotkey_key[24] = "volup";
  char hotkey_action[32] = "speed";
  char hotkey_arg[96] = "2.0";
  char trainer_name[64] = "MyTrainer";
  char trainer_pkg[128] = "";
  char trainer_path[192] = "/data/local/tmp/memdbg_trainer.trainer";
  char script_path[192] = "/data/local/tmp/memdbg_example.lua";
  // Lua 示例分类：0=仅显示分类条，1..9=该分类子示例（默认 1 读写，进页即可见）
  int lua_ex_cat = 1;

  // 内存地图
  std::vector<mem::Region> maps_cache;
  char maps_filter[64] = "";

  // 指针扫描 / 模板
  char ptr_target[32] = "0x0";
  int ptr_level = 3;
  int ptr_maxoff = 0x1000;
  bool ptr_static = true;
  bool ptr_scanning = false;
  std::vector<mem::PtrChain> ptr_results;
  int ptr_selected = -1;
  char ptr_template_buf[256] = "";  // 手动模板 / 导入
  char ptr_save_path[128] = "/data/local/tmp/memdbg_ptrs.txt";

  // 结构体解析
  mem::Structure structure;
  int struct_field_type = (int)mem::FieldType::I32;
  char struct_field_name[48] = "field";
  char struct_base_buf[32] = "0x0";
  char struct_save_path[128] = "/data/local/tmp/memdbg_struct.txt";
  int struct_auto_n = 16;

  // 工具
  char dump_size_buf[32] = "4096";

  // 多浮动窗口（可拖拽缩放）
  enum class FloatKind : int {
    Disasm = 0,
    PseudoC,
    Breakpoints,
    HitLog,
    HexPeek,
    Registers,
    Threads,
    Modules,
    StackTrace,
  };
  struct FloatWin {
    int id = 0;
    bool open = true;
    FloatKind kind = FloatKind::Disasm;
    char title[96]{};
    ImVec2 pos{80.f, 120.f};
    ImVec2 size{420.f, 360.f};
    uintptr_t addr = 0;
    std::vector<mem::Insn> insns;
    std::string pseudo;
    std::vector<mem::Xref> xrefs;
    char sym_near[96]{};
    int view_tab = 0;  // 0汇编 1伪C 2xrefs
    // 交互
    bool dragging = false;
    bool resizing = false;
    int resize_mask = 0;  // bit0=宽 bit1=高
    ImVec2 grab_mouse{};
    ImVec2 grab_pos{};
    ImVec2 grab_size{};
  };
  std::vector<FloatWin> float_wins;
  int next_win_id = 1;
  mem::BpHit last_hit{};
  bool has_hit = false;
  bool show_bp_manager = false;  // 快捷

  char status[160] = "MemDbg · perf 断点 · 多窗口";
  float toast_t = 0.f;
  char toast_msg[128]{};

  AnimBtn btns[128]{};
  char* ime_target = nullptr;

  bool want_exit = false;    // 延后退出，避免绘制中途 shutdown 闪退
  float cfg_save_acc = 0.f;  // 周期性保存 UI 配置

  void toast(const char* msg) {
    std::snprintf(toast_msg, sizeof(toast_msg), "%s", msg);
    toast_t = 1.f;
    std::snprintf(status, sizeof(status), "%s", msg);
  }
};

// UI 配置路径（私有，退出可保留）
static const char* kUiCfgPath = "/data/local/tmp/.mdbg_ui.cfg";

void save_ui_config(const Ui& ui) {
  FILE* f = std::fopen(kUiCfgPath, "w");
  if (!f) return;
  std::fprintf(f, "ball_x=%.1f\nball_y=%.1f\n", ui.ball_pos.x, ui.ball_pos.y);
  std::fprintf(f, "panel_x=%.1f\npanel_y=%.1f\n", ui.panel_pos.x, ui.panel_pos.y);
  std::fprintf(f, "panel_w=%.1f\npanel_h=%.1f\n", ui.panel_size.x,
               ui.panel_size.y);
  std::fprintf(f, "tab=%d\nscan_sub=%d\naddr_sub=%d\nanalyze_sub=%d\n"
                  "debug_sub=%d\nauto_sub=%d\n",
               (int)ui.tab, ui.scan_sub, ui.addr_sub, ui.analyze_sub,
               ui.debug_sub, ui.auto_sub);
  std::fprintf(f, "vtype=%d\nsmode=%d\nregion=%d\n", (int)ui.vtype,
               (int)ui.smode, (int)ui.region_filter);
  std::fprintf(f, "skip_no_icon=%d\ntencent_only=%d\nfilter_asm_noise=%d\n",
               (int)ui.skip_no_icon, (int)ui.tencent_only,
               (int)ui.filter_asm_noise);
  std::fprintf(f, "result_page_size=%d\nspeed_mult=%.2f\n", ui.result_page_size,
               ui.speed_mult);
  std::fprintf(f, "process_filter=%s\n", ui.process_filter);
  std::fprintf(f, "auto_regs_on_hit=%d\n", (int)ui.auto_regs_on_hit);
  std::fclose(f);
  chmod(kUiCfgPath, 0600);
}

void load_ui_config(Ui& ui) {
  FILE* f = std::fopen(kUiCfgPath, "r");
  if (!f) return;
  char line[256];
  while (std::fgets(line, sizeof(line), f)) {
    char key[64]{};
    char val[192]{};
    if (std::sscanf(line, "%63[^=]=%191[^\n]", key, val) != 2) continue;
    auto fval = [&]() { return (float)std::atof(val); };
    auto ival = [&]() { return std::atoi(val); };
    if (!std::strcmp(key, "ball_x")) ui.ball_pos.x = fval();
    else if (!std::strcmp(key, "ball_y")) ui.ball_pos.y = fval();
    else if (!std::strcmp(key, "panel_x")) {
      ui.panel_pos.x = fval();
      ui.panel_geom_init = true;
    } else if (!std::strcmp(key, "panel_y")) ui.panel_pos.y = fval();
    else if (!std::strcmp(key, "panel_w")) ui.panel_size.x = fval();
    else if (!std::strcmp(key, "panel_h")) ui.panel_size.y = fval();
    else if (!std::strcmp(key, "tab")) {
      int t = ival();
      if (t >= 0 && t < (int)Tab::COUNT) ui.tab = (Tab)t;
    } else if (!std::strcmp(key, "scan_sub")) ui.scan_sub = ival();
    else if (!std::strcmp(key, "addr_sub")) ui.addr_sub = ival();
    else if (!std::strcmp(key, "analyze_sub")) ui.analyze_sub = ival();
    else if (!std::strcmp(key, "debug_sub")) ui.debug_sub = ival();
    else if (!std::strcmp(key, "auto_sub")) ui.auto_sub = ival();
    else if (!std::strcmp(key, "vtype")) {
      int t = ival();
      if (t >= 0 && t < (int)ValueType::COUNT) ui.vtype = (ValueType)t;
    } else if (!std::strcmp(key, "smode")) {
      int t = ival();
      if (t >= 0 && t < (int)SearchMode::COUNT) ui.smode = (SearchMode)t;
    } else if (!std::strcmp(key, "region")) {
      int t = ival();
      if (t >= 0 && t < (int)mem::RegionFilter::COUNT)
        ui.region_filter = (mem::RegionFilter)t;
    } else if (!std::strcmp(key, "skip_no_icon"))
      ui.skip_no_icon = ival() != 0;
    else if (!std::strcmp(key, "tencent_only"))
      ui.tencent_only = ival() != 0;
    else if (!std::strcmp(key, "filter_asm_noise"))
      ui.filter_asm_noise = ival() != 0;
    else if (!std::strcmp(key, "result_page_size")) {
      int n = ival();
      if (n == 50 || n == 100 || n == 200) ui.result_page_size = n;
    } else if (!std::strcmp(key, "speed_mult")) ui.speed_mult = fval();
    else if (!std::strcmp(key, "process_filter"))
      std::snprintf(ui.process_filter, sizeof(ui.process_filter), "%s", val);
    else if (!std::strcmp(key, "auto_regs_on_hit"))
      ui.auto_regs_on_hit = ival() != 0;
  }
  std::fclose(f);
}

/** 仅标记退出；重清理放到帧末 do_exit_cleanup，避免绘制中途闪退 */
void request_exit(Ui& ui) {
  if (ui.want_exit) return;
  ui.want_exit = true;
  ui.want_open = false;
  ui.toast("正在退出…");
}

void do_exit_cleanup(Ui& ui) {
  // 先保存 UI 配置，再清残留（cleanup 会跳过 .mdbg_ui.cfg）
  save_ui_config(ui);
  mem::script_shutdown();
  mem::soft_bp_clear_all();
  mem::bp_shutdown();
  mem::speed_disable();
  mem::detach();
  mem::icon_cleanup_traces();
  // 删除 cwd 下误生成的 imgui.ini（我们已禁用自动保存，仅扫残留）
  unlink("imgui.ini");
  unlink("./imgui.ini");
  unlink("/data/local/tmp/imgui.ini");
  ui.running = false;
}

mem::ValType to_mem_type(ValueType t) { return (mem::ValType)(int)t; }
mem::ScanMode to_mem_mode(SearchMode m) { return (mem::ScanMode)(int)m; }

void sync_results_from_engine(Ui& ui) {
  ui.result_count = (int)mem::result_count();
  ui.search_round = mem::scan_round();
  // 分页：纠正页码
  int pages = ui.result_count > 0
                  ? (ui.result_count + ui.result_page_size - 1) /
                        ui.result_page_size
                  : 1;
  if (ui.result_page < 0) ui.result_page = 0;
  if (ui.result_page >= pages) ui.result_page = pages - 1;

  size_t offset = (size_t)ui.result_page * (size_t)ui.result_page_size;
  std::vector<mem::Match> ms;
  mem::copy_results_range(ms, offset, (size_t)ui.result_page_size);
  mem::refresh_result_values(ms);
  ui.results.clear();
  ui.results.reserve(ms.size());
  auto vt = mem::last_scan_type();
  // 若 UI 类型与上次扫描一致更好
  if ((int)ui.vtype < (int)mem::ValType::COUNT)
    vt = to_mem_type(ui.vtype);
  for (auto& m : ms) {
    UiResult r;
    r.addr = m.addr;
    r.frozen = m.frozen;
    r.bits = m.value_bits;
    if (mem::is_pattern_type(vt) || vt == mem::ValType::StrUtf8 ||
        vt == mem::ValType::StrUtf16) {
      mem::format_at(vt, m.addr, m.match_len, r.value, sizeof(r.value));
    } else {
      mem::format_value(vt, m.value_bits, r.value, sizeof(r.value));
    }
    ui.results.push_back(r);
  }
}

// 包名 → GPU 纹理缓存（-1 标记加载失败）
struct IconCacheEntry {
  vkeng::Texture tex{};
  bool tried = false;
  bool failed = false;
};
static std::unordered_map<std::string, IconCacheEntry> g_icon_tex;

static vkeng::Texture* ensure_pkg_icon(const std::string& pkg) {
  if (pkg.empty()) return nullptr;
  auto& e = g_icon_tex[pkg];
  if (e.failed) return nullptr;
  if (e.tex.valid()) return &e.tex;
  if (e.tried) return nullptr;
  e.tried = true;
  std::vector<uint8_t> rgba;
  int w = 0, h = 0;
  if (!mem::load_app_icon_rgba(pkg, rgba, w, h) || rgba.empty() || w <= 0) {
    e.failed = true;
    return nullptr;
  }
  // 缩到 ≤96 以省显存（最近邻）
  int tw = w, th = h;
  if (w > 96 || h > 96) {
    tw = th = 48;
    std::vector<uint8_t> small((size_t)tw * th * 4);
    for (int y = 0; y < th; ++y) {
      int sy = y * h / th;
      for (int x = 0; x < tw; ++x) {
        int sx = x * w / tw;
        size_t di = ((size_t)y * tw + x) * 4;
        size_t si = ((size_t)sy * w + sx) * 4;
        small[di] = rgba[si];
        small[di + 1] = rgba[si + 1];
        small[di + 2] = rgba[si + 2];
        small[di + 3] = rgba[si + 3];
      }
    }
    e.tex = vkeng::create_texture_rgba(small.data(), tw, th);
  } else {
    e.tex = vkeng::create_texture_rgba(rgba.data(), w, h);
  }
  if (!e.tex.valid()) {
    e.failed = true;
    return nullptr;
  }
  return &e.tex;
}

void refresh_process_list(Ui& ui) {
  mem::icon_cache_refresh();  // 刷新 packages.list / apk 索引
  mem::ProcListOptions opt;
  opt.filter = ui.process_filter[0] ? ui.process_filter : nullptr;
  opt.skip_system = false;
  opt.skip_no_icon = ui.skip_no_icon;
  opt.tencent_only = ui.tencent_only;
  opt.fuzzy_filter = true;
  ui.procs = mem::list_processes(opt);
  ui.selected_list = -1;

  // PackageManager 导出图标（app_process IconDump）— 比 APK 扫更完整
  // 仅对当前列表中尚无缓存的包名按需导出，避免每次全量
  {
    std::vector<std::string> need;
    need.reserve(ui.procs.size());
    for (auto& pi : ui.procs) {
      if (pi.package.empty() || pi.package.find('.') == std::string::npos)
        continue;
      char path[400];
      std::snprintf(path, sizeof(path), "%s/%s.png", mem::icon_pm_cache_dir(),
                    pi.package.c_str());
      struct stat st {};
      if (stat(path, &st) != 0 || st.st_size < 64) need.push_back(pi.package);
    }
    // 去重
    std::sort(need.begin(), need.end());
    need.erase(std::unique(need.begin(), need.end()), need.end());
    int dumped = 0;
    if (!need.empty()) {
      // 分批，避免一次参数过长；每批最多 40
      for (size_t i = 0; i < need.size(); i += 40) {
        size_t n = std::min((size_t)40, need.size() - i);
        std::vector<std::string> batch(need.begin() + (std::ptrdiff_t)i,
                                       need.begin() + (std::ptrdiff_t)(i + n));
        int r = mem::icon_pm_dump(batch, 20000);
        if (r > 0) dumped += r;
        if (r < 0) break;  // dex 不存在则不再试
      }
      // 清空 GPU 图标失败缓存，允许重新加载
      g_icon_tex.clear();
    }
    char buf[160];
    std::snprintf(buf, sizeof(buf), "进程 %d · PM图标+%d%s%s",
                  (int)ui.procs.size(), dumped,
                  ui.skip_no_icon ? " ·需图标" : "",
                  ui.tencent_only ? " ·仅腾讯" : "");
    ui.toast(buf);
  }
}

/** 绘制进程图标：优先 APK 真图标，失败则首字母色块 */
void draw_proc_icon(ImDrawList* dl, ImVec2 p, float sz, const mem::ProcInfo& pi) {
  vkeng::Texture* tex = ensure_pkg_icon(pi.package);
  if (tex && tex->valid()) {
    ImTextureID tid = (ImTextureID)tex->id;
    dl->AddImageRounded(tid, p, ImVec2(p.x + sz, p.y + sz), ImVec2(0, 0),
                        ImVec2(1, 1), IM_COL32_WHITE, sz * 0.28f);
    dl->AddRect(p, ImVec2(p.x + sz, p.y + sz), IM_COL32(255, 255, 255, 140),
                sz * 0.28f, 0, 1.2f);
    return;
  }

  ImU32 bg;
  if (pi.is_tencent) {
    if (pi.package.find("mm") != std::string::npos ||
        pi.package.find("wechat") != std::string::npos)
      bg = IM_COL32(7, 193, 96, 255);
    else if (pi.package.find("mobileqq") != std::string::npos ||
             pi.package.find("tim") != std::string::npos)
      bg = IM_COL32(18, 183, 245, 255);
    else
      bg = IM_COL32(255, 120, 80, 255);
  } else if (pi.has_icon) {
    int c = pi.icon_color;
    int r = 140 + ((c >> 16) & 0x7F);
    int g = 120 + ((c >> 8) & 0x7F);
    int b = 140 + (c & 0x7F);
    bg = IM_COL32(r, g, b, 255);
  } else {
    bg = IM_COL32(200, 180, 190, 220);
  }
  dl->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), bg, sz * 0.28f);
  dl->AddRect(p, ImVec2(p.x + sz, p.y + sz), IM_COL32(255, 255, 255, 160),
              sz * 0.28f, 0, 1.2f);
  char letter[4] = "?";
  if (!pi.package.empty()) {
    auto dot = pi.package.find_last_of('.');
    char ch = (dot != std::string::npos && dot + 1 < pi.package.size())
                  ? pi.package[dot + 1]
                  : pi.package[0];
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    letter[0] = ch;
    letter[1] = 0;
  }
  ImVec2 ts = ImGui::CalcTextSize(letter);
  dl->AddText(ImVec2(p.x + (sz - ts.x) * 0.5f, p.y + (sz - ts.y) * 0.5f),
              IM_COL32(255, 255, 255, 255), letter);
}

/** 汇编语法着色绘制一行；cur=窗口当前地址，pc=调试 PC（0=无） */
void draw_insn_colored(const mem::Insn& in, uintptr_t cur_addr = 0,
                       uintptr_t pc_addr = 0) {
  const uintptr_t a = in.addr & ~3ull;
  const uintptr_t cur = cur_addr & ~3ull;
  const uintptr_t pc = pc_addr & ~3ull;
  const bool is_pc = pc && a == pc;
  const bool is_cur = cur && a == cur && !is_pc;
  const bool is_near =
      cur && !is_pc && !is_cur && a + 16 >= cur && a <= cur + 16;

  if (is_pc || is_cur) {
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float h = ImGui::GetTextLineHeightWithSpacing();
    ImU32 bg = is_pc ? IM_COL32(255, 80, 120, 55) : IM_COL32(80, 160, 255, 48);
    ImGui::GetWindowDrawList()->AddRectFilled(
        p0, ImVec2(p0.x + w, p0.y + h), bg, 4.f);
  }

  if (in.is_label && in.label[0]) {
    ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.35f, 1.f), "%s:", in.label);
    if (in.xref_in > 0) {
      ImGui::SameLine(0, 6);
      ImGui::TextColored(theme::muted, "  ; xref in=%d", in.xref_in);
    }
  }

  // 行标记：PC / 当前 / 空白
  if (is_pc)
    ImGui::TextColored(ImVec4(1.f, 0.35f, 0.5f, 1.f), "PC>");
  else if (is_cur)
    ImGui::TextColored(ImVec4(0.3f, 0.65f, 1.f, 1.f), ">>");
  else if (is_near)
    ImGui::TextColored(theme::muted, " |");
  else
    ImGui::TextDisabled("  ");
  ImGui::SameLine(0, 4);

  // 地址（当前/PC 加亮）
  ImVec4 ac = is_pc    ? ImVec4(1.f, 0.4f, 0.55f, 1.f)
              : is_cur ? ImVec4(0.35f, 0.7f, 1.f, 1.f)
                       : ImVec4(0.65f, 0.55f, 0.60f, 1.f);
  ImGui::TextColored(ac, "%08llX", (unsigned long long)in.addr);
  ImGui::SameLine(0, 8);
  // 机器码
  ImGui::TextColored(ImVec4(0.75f, 0.70f, 0.72f, 0.85f), "%08X", in.raw);
  ImGui::SameLine(0, 10);

  // 助记符颜色
  ImVec4 mc(0.55f, 0.35f, 0.55f, 1.f);  // 默认紫
  switch (in.cat) {
    case mem::InsnCat::Branch:
      mc = ImVec4(0.95f, 0.35f, 0.55f, 1.f);  // 粉红跳转
      break;
    case mem::InsnCat::Load:
      mc = ImVec4(0.25f, 0.55f, 0.95f, 1.f);  // 蓝 读
      break;
    case mem::InsnCat::Store:
      mc = ImVec4(0.95f, 0.55f, 0.25f, 1.f);  // 橙 写
      break;
    case mem::InsnCat::Mov:
      mc = ImVec4(0.55f, 0.45f, 0.90f, 1.f);  // 紫 mov
      break;
    case mem::InsnCat::Compare:
      mc = ImVec4(0.90f, 0.50f, 0.20f, 1.f);
      break;
    case mem::InsnCat::System:
      mc = ImVec4(0.90f, 0.25f, 0.35f, 1.f);
      break;
    case mem::InsnCat::Alu:
      mc = ImVec4(0.45f, 0.35f, 0.75f, 1.f);
      break;
    case mem::InsnCat::Float:
      mc = ImVec4(0.30f, 0.75f, 0.70f, 1.f);
      break;
    case mem::InsnCat::Noise:
      mc = ImVec4(0.70f, 0.65f, 0.68f, 0.55f);
      break;
    default:
      break;
  }
  ImGui::TextColored(mc, "%-6s", in.mnem);
  ImGui::SameLine(0, 6);

  // 操作数：寄存器绿、立即数橙、其余可可色
  const char* s = in.ops;
  if (!s || !s[0]) {
    ImGui::NewLine();
    return;
  }
  // 简单分词着色
  char token[64];
  int ti = 0;
  auto is_reg_start = [](char c) {
    return c == 'x' || c == 'w' || c == 's' || c == 'd' || c == 'v' || c == 'b';
  };
  for (const char* p = s; *p; ++p) {
    char c = *p;
    if (c == ',' || c == ' ' || c == '[' || c == ']' || c == '!' || c == '#') {
      // flush token first
      if (ti > 0) {
        token[ti] = 0;
        ImVec4 col(0.38f, 0.28f, 0.35f, 1.f);
        if (token[0] == '#' || (token[0] == '0' && token[1] == 'x'))
          col = ImVec4(0.95f, 0.50f, 0.25f, 1.f);  // imm
        else if (is_reg_start(token[0]) || std::strcmp(token, "sp") == 0 ||
                 std::strcmp(token, "lr") == 0 || std::strcmp(token, "fp") == 0 ||
                 std::strcmp(token, "wsp") == 0 || std::strcmp(token, "xzr") == 0 ||
                 std::strcmp(token, "wzr") == 0)
          col = ImVec4(0.20f, 0.65f, 0.45f, 1.f);  // reg
        ImGui::SameLine(0, 0);
        ImGui::TextColored(col, "%s", token);
        ti = 0;
      }
      char punct[2] = {c, 0};
      ImGui::SameLine(0, 0);
      ImGui::TextColored(ImVec4(0.70f, 0.55f, 0.60f, 1.f), "%s", punct);
    } else {
      if (ti < (int)sizeof(token) - 1) token[ti++] = c;
    }
  }
  if (ti > 0) {
    token[ti] = 0;
    ImVec4 col(0.38f, 0.28f, 0.35f, 1.f);
    if (token[0] == '#' || (token[0] == '0' && token[1] == 'x'))
      col = ImVec4(0.95f, 0.50f, 0.25f, 1.f);
    else if (is_reg_start(token[0]) || std::strcmp(token, "sp") == 0 ||
             std::strcmp(token, "lr") == 0 || std::strcmp(token, "fp") == 0)
      col = ImVec4(0.20f, 0.65f, 0.45f, 1.f);
    ImGui::SameLine(0, 0);
    ImGui::TextColored(col, "%s", token);
  }
}

// ── 多窗口：新建 ──────────────────────────────────────────
Ui::FloatWin* spawn_float(Ui& ui, Ui::FloatKind kind, uintptr_t addr,
                          const char* title_override = nullptr) {
  auto disp = vkeng::display();
  Ui::FloatWin w;
  w.id = ui.next_win_id++;
  w.open = true;
  w.kind = kind;
  w.addr = addr;
  // 错落排布，避免重叠
  float ox = 40.f + (float)((ui.float_wins.size() % 4) * 28);
  float oy = 80.f + (float)((ui.float_wins.size() % 5) * 36);
  w.pos = ImVec2(ox, oy);
  w.size = ImVec2(std::min(440.f, (float)disp.width * 0.55f),
                  std::min(380.f, (float)disp.height * 0.50f));

  if (kind == Ui::FloatKind::Disasm || kind == Ui::FloatKind::PseudoC) {
    mem::DisasmOptions dop;
    dop.filter_noise = ui.filter_asm_noise;
    dop.resolve_symbols = true;
    dop.build_xrefs = true;
    if (mem::sym_count() == 0) mem::sym_refresh();
    w.insns = mem::disasm_function(addr, 160, dop);
    if (w.insns.empty()) w.insns = mem::disasm_at(addr, 64, dop);
    w.pseudo = mem::insns_to_cfg_pseudo_c(w.insns, addr);
    w.xrefs = mem::build_xrefs(w.insns);
    mem::SymInfo si{};
    if (mem::sym_lookup(addr, si) || mem::sym_nearest(addr, si, 0x2000))
      std::snprintf(w.sym_near, sizeof(w.sym_near), "%s +0x%llX (%s)", si.name,
                    (unsigned long long)(addr >= si.addr ? addr - si.addr : 0),
                    si.module);
    w.view_tab = (kind == Ui::FloatKind::PseudoC) ? 1 : 0;
    if (title_override)
      std::snprintf(w.title, sizeof(w.title), "%s", title_override);
    else if (si.name[0])
      std::snprintf(w.title, sizeof(w.title), "%s #%d", si.name, w.id);
    else
      std::snprintf(w.title, sizeof(w.title), "反汇编 0x%llX #%d",
                    (unsigned long long)addr, w.id);
  } else if (kind == Ui::FloatKind::Breakpoints) {
    std::snprintf(w.title, sizeof(w.title), "断点管理 #%d", w.id);
    w.size = ImVec2(380.f, 320.f);
  } else if (kind == Ui::FloatKind::HitLog) {
    std::snprintf(w.title, sizeof(w.title), "断点命中日志 #%d", w.id);
    w.size = ImVec2(400.f, 280.f);
  } else if (kind == Ui::FloatKind::HexPeek) {
    std::snprintf(w.title, sizeof(w.title), "Hex 0x%llX #%d",
                  (unsigned long long)addr, w.id);
    w.size = ImVec2(360.f, 260.f);
  } else if (kind == Ui::FloatKind::Registers) {
    std::snprintf(w.title, sizeof(w.title), "寄存器 #%d", w.id);
    w.size = ImVec2(400.f, 420.f);
  } else if (kind == Ui::FloatKind::Threads) {
    std::snprintf(w.title, sizeof(w.title), "线程列表 #%d", w.id);
    w.size = ImVec2(360.f, 320.f);
  } else if (kind == Ui::FloatKind::Modules) {
    std::snprintf(w.title, sizeof(w.title), "模块列表 #%d", w.id);
    w.size = ImVec2(420.f, 360.f);
  } else if (kind == Ui::FloatKind::StackTrace) {
    std::snprintf(w.title, sizeof(w.title), "调用栈 #%d", w.id);
    w.size = ImVec2(400.f, 300.f);
  }
  ui.float_wins.push_back(std::move(w));
  return &ui.float_wins.back();
}

static void refill_disasm_win(Ui::FloatWin* w, uintptr_t addr, bool as_function,
                              bool filter_noise) {
  if (!w) return;
  mem::DisasmOptions dop;
  dop.filter_noise = filter_noise;
  dop.resolve_symbols = true;
  dop.build_xrefs = true;
  if (mem::sym_count() == 0) mem::sym_refresh();
  if (as_function)
    w->insns = mem::disasm_function(addr, 160, dop);
  else
    w->insns = mem::disasm_at(addr, 64, dop);
  if (w->insns.empty() && as_function)
    w->insns = mem::disasm_at(addr, 64, dop);
  w->pseudo = mem::insns_to_cfg_pseudo_c(w->insns, addr);
  w->xrefs = mem::build_xrefs(w->insns);
  mem::SymInfo si{};
  w->sym_near[0] = 0;
  if (mem::sym_lookup(addr, si) || mem::sym_nearest(addr, si, 0x2000))
    std::snprintf(w->sym_near, sizeof(w->sym_near), "%s +0x%llX (%s)", si.name,
                  (unsigned long long)(addr >= si.addr ? addr - si.addr : 0),
                  si.module);
}

void open_analysis(Ui& ui, uintptr_t addr, bool as_function) {
  auto* w = spawn_float(ui, Ui::FloatKind::Disasm, addr, nullptr);
  if (!w) return;
  refill_disasm_win(w, addr, as_function, ui.filter_asm_noise);
  if (as_function) {
    if (w->sym_near[0])
      std::snprintf(w->title, sizeof(w->title), "函数 %s #%d", w->sym_near,
                    w->id);
    else
      std::snprintf(w->title, sizeof(w->title), "函数 0x%llX #%d",
                    (unsigned long long)addr, w->id);
  } else {
    std::snprintf(w->title, sizeof(w->title), "反汇编 0x%llX #%d",
                  (unsigned long long)addr, w->id);
  }
  if (w->insns.empty())
    ui.toast("反汇编失败");
  else {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "窗口#%d · %d 条 · xref %d · sym %zu",
                  w->id, (int)w->insns.size(), (int)w->xrefs.size(),
                  mem::sym_count());
    ui.toast(buf);
  }
}

void open_pseudo_window(Ui& ui, uintptr_t addr) {
  auto* w = spawn_float(ui, Ui::FloatKind::PseudoC, addr, nullptr);
  if (!w) return;
  refill_disasm_win(w, addr, true, ui.filter_asm_noise);
  w->view_tab = 1;
  std::snprintf(w->title, sizeof(w->title), "伪C 0x%llX #%d",
                (unsigned long long)addr, w->id);
  ui.toast("已打开伪C窗口");
}

/** 手动处理浮动窗拖动/缩放（触摸友好：右边改宽、下边改高、角双轴） */
void update_float_win_geom(Ui::FloatWin& w) {
  ImGuiIO& io = ImGui::GetIO();
  const bool down = io.MouseDown[0];
  const ImVec2 m = io.MousePos;
  auto disp = vkeng::display();
  const float sw = (float)disp.width, sh = (float)disp.height;
  const float min_w = 220.f, min_h = 160.f;
  const float edge = 22.f;
  const float corner = 28.f;

  auto in_title = [&](ImVec2 p) {
    return p.x >= w.pos.x && p.x < w.pos.x + w.size.x - 36.f &&
           p.y >= w.pos.y && p.y < w.pos.y + 28.f;
  };
  // bit0=宽 bit1=高
  auto hit_resize = [&](ImVec2 p) -> int {
    const float r = w.pos.x + w.size.x;
    const float b = w.pos.y + w.size.y;
    if (p.x < w.pos.x - 2.f || p.x >= r + 8.f || p.y < w.pos.y - 2.f ||
        p.y >= b + 8.f)
      return 0;
    const bool near_r = p.x >= r - edge && p.x < r + 8.f;
    const bool near_b = p.y >= b - edge && p.y < b + 8.f;
    if ((p.x >= r - corner && p.y >= b - corner) || (near_r && near_b))
      return 3;
    if (near_r) return 1;
    if (near_b) return 2;
    return 0;
  };

  if (!down) {
    w.dragging = w.resizing = false;
    w.resize_mask = 0;
    return;
  }

  if (!w.dragging && !w.resizing && io.MouseClicked[0]) {
    int mask = hit_resize(m);
    if (mask) {
      w.resizing = true;
      w.resize_mask = mask;
      w.grab_mouse = m;
      w.grab_size = w.size;
      w.grab_pos = w.pos;
    } else if (in_title(m)) {
      w.dragging = true;
      w.grab_mouse = m;
      w.grab_pos = w.pos;
    }
  }

  if (w.dragging) {
    w.pos.x = w.grab_pos.x + (m.x - w.grab_mouse.x);
    w.pos.y = w.grab_pos.y + (m.y - w.grab_mouse.y);
    w.pos.x = clampf(w.pos.x, 0.f, sw - 40.f);
    w.pos.y = clampf(w.pos.y, 0.f, sh - 40.f);
  } else if (w.resizing) {
    float max_w = std::max(min_w, sw - w.pos.x - 4.f);
    float max_h = std::max(min_h, sh - w.pos.y - 4.f);
    if (w.resize_mask & 1) {
      w.size.x =
          clampf(w.grab_size.x + (m.x - w.grab_mouse.x), min_w, max_w);
    }
    if (w.resize_mask & 2) {
      w.size.y =
          clampf(w.grab_size.y + (m.y - w.grab_mouse.y), min_h, max_h);
    }
  }
}

void draw_float_win_content(Ui& ui, Ui::FloatWin& w) {
  if (w.kind == Ui::FloatKind::Disasm || w.kind == Ui::FloatKind::PseudoC) {
    if (chip_button("汇编", w.view_tab == 0 ? theme::accent : theme::idle))
      w.view_tab = 0;
    ImGui::SameLine();
    if (chip_button("伪C", w.view_tab == 1 ? theme::accent : theme::idle))
      w.view_tab = 1;
    ImGui::SameLine();
    if (chip_button("xrefs", w.view_tab == 2 ? theme::accent : theme::idle))
      w.view_tab = 2;
    ImGui::SameLine();
    if (chip_button("刷新", theme::teal)) {
      refill_disasm_win(&w, w.addr, true, ui.filter_asm_noise);
    }
    ImGui::SameLine();
    if (chip_button("符号", theme::secondary)) {
      int n = mem::sym_refresh();
      refill_disasm_win(&w, w.addr, true, ui.filter_asm_noise);
      char b[48];
      std::snprintf(b, sizeof(b), "符号 %d", n);
      ui.toast(b);
    }
    ImGui::SameLine();
    if (chip_button("再开伪C窗", theme::accent2))
      open_pseudo_window(ui, w.addr);
    ImGui::Checkbox("过滤无用指令(nop/pac/bti…)", &ui.filter_asm_noise);
    // 当前地址 / PC 上下文
    uintptr_t pc_now = 0;
    if (mem::dbg_is_paused() || ui.has_hit) {
      mem::Regs rr{};
      if (mem::dbg_regs_read(rr) && rr.pc)
        pc_now = rr.pc;
      else if (ui.has_hit && ui.last_hit.pc)
        pc_now = ui.last_hit.pc;
    }
    uintptr_t edit_a = 0;
    mem::parse_addr(ui.edit_addr, edit_a);
    ImGui::TextColored(theme::accent, "当前>> 0x%llX",
                       (unsigned long long)w.addr);
    ImGui::SameLine();
    if (pc_now)
      ImGui::TextColored(ImVec4(1.f, 0.4f, 0.55f, 1.f), "PC> 0x%llX",
                         (unsigned long long)pc_now);
    else
      ImGui::TextDisabled("PC> —");
    ImGui::SameLine();
    if (edit_a)
      ImGui::TextColored(theme::muted, "编辑 0x%llX",
                         (unsigned long long)edit_a);
    if (w.sym_near[0])
      ImGui::TextColored(theme::peach, "sym: %s", w.sym_near);
    ImGui::TextColored(theme::muted,
                       ">>当前  PC红  |附近 · 跳转粉|读蓝|写橙 · sym%zu xref%d",
                       mem::sym_count(), (int)w.xrefs.size());
    ImGui::Separator();
    if (ImGui::BeginChild("body", ImVec2(0, 0), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
      if (w.view_tab == 0) {
        if (w.insns.empty()) ImGui::TextDisabled("无指令（或已全部过滤）");
        for (auto& in : w.insns) {
          draw_insn_colored(in, w.addr, pc_now);
          if (in.target_name[0] && (in.is_branch || in.is_call)) {
            ImGui::SameLine(0, 8);
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.40f, 0.95f), "-> %s",
                               in.target_name);
          }
        }
      } else if (w.view_tab == 1) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::text);
        ImGui::TextUnformatted(w.pseudo.empty() ? "// empty" : w.pseudo.c_str());
        ImGui::PopStyleColor();
      } else {
        if (w.xrefs.empty())
          ImGui::TextDisabled("本窗口指令流无跳转/调用 xref");
        for (auto& x : w.xrefs) {
          ImGui::TextColored(theme::peach, "0x%llX",
                             (unsigned long long)x.from);
          ImGui::SameLine(0, 6);
          ImGui::TextColored(theme::muted, "-%s->", x.kind);
          ImGui::SameLine(0, 6);
          ImGui::Text("0x%llX", (unsigned long long)x.to);
          ImGui::SameLine(0, 8);
          ImGui::TextColored(theme::accent, "%s", x.mnem);
          const char* tn = mem::sym_name_at(x.to);
          if (tn && tn[0]) {
            ImGui::SameLine(0, 8);
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.4f, 1.f), "%s", tn);
          }
          ImGui::SameLine(0, 8);
          if (chip_button("去", theme::teal)) open_analysis(ui, x.to, true);
        }
      }
    }
    ImGui::EndChild();
  } else if (w.kind == Ui::FloatKind::Breakpoints) {
    ImGui::TextColored(theme::muted, "后端: %s · %s", mem::bp_backend_name(),
                       mem::bp_status());
    if (chip_button("开始监听", theme::success)) {
      mem::bp_init();
      mem::bp_arm_and_continue();
      ui.toast(mem::bp_status());
    }
    ImGui::SameLine();
    if (chip_button("全部清除", theme::danger)) {
      mem::bp_clear_all();
      ui.toast("已清除");
    }
    ImGui::SameLine();
    if (chip_button("继续", theme::teal)) mem::bp_continue();
    ImGui::Separator();
    auto list = mem::bp_list();
    if (list.empty()) ImGui::TextDisabled("无断点 · 在结果页添加");
    for (auto& b : list) {
      ImGui::PushID(b.id);
      ImGui::Text("#%d %s 0x%llX sz=%d hit=%llu %s", b.id,
                  mem::bp_type_name(b.type), (unsigned long long)b.addr, b.size,
                  (unsigned long long)b.hit_count, b.enabled ? "ON" : "off");
      if (chip_button(b.enabled ? "禁用" : "启用", theme::warn))
        mem::bp_enable(b.id, !b.enabled);
      ImGui::SameLine();
      if (chip_button("删除", theme::danger)) mem::bp_clear(b.id);
      ImGui::SameLine();
      if (chip_button("反汇编", theme::peach)) open_analysis(ui, b.addr, false);
      ImGui::PopID();
    }
  } else if (w.kind == Ui::FloatKind::HitLog) {
    if (chip_button("清空日志", theme::danger)) mem::bp_clear_hit_log();
    ImGui::SameLine();
    if (ui.has_hit && chip_button("反汇编最近PC", theme::peach))
      open_analysis(ui, ui.last_hit.pc ? ui.last_hit.pc : ui.last_hit.watch_addr,
                    true);
    ImGui::Separator();
    auto log = mem::bp_hit_log(40);
    if (log.empty()) ImGui::TextDisabled("暂无命中");
    for (size_t i = 0; i < log.size(); ++i) {
      auto& h = log[i];
      ImGui::PushID((int)i);
      ImGui::TextWrapped("%s", h.msg);
      if (h.pc && chip_button("反汇编IP", theme::peach))
        open_analysis(ui, h.pc, true);
      ImGui::SameLine();
      if (h.watch_addr && chip_button("数据址", theme::secondary))
        open_analysis(ui, h.watch_addr, false);
      ImGui::PopID();
    }
  } else if (w.kind == Ui::FloatKind::HexPeek) {
    uint8_t buf[64]{};
    if (mem::is_attached() && mem::read_mem(w.addr, buf, sizeof(buf))) {
      for (int row = 0; row < 4; ++row) {
        ImGui::Text("%08llX", (unsigned long long)(w.addr + row * 16));
        ImGui::SameLine();
        for (int c = 0; c < 16; ++c) {
          ImGui::Text("%02X", buf[row * 16 + c]);
          if (c != 15) ImGui::SameLine(0, 2);
        }
      }
    } else {
      ImGui::TextDisabled("无法读取");
    }
    if (chip_button("反汇编此处", theme::peach))
      open_analysis(ui, w.addr, false);
  } else if (w.kind == Ui::FloatKind::Registers) {
    if (chip_button("刷新", theme::accent)) {
      mem::dbg_regs_read(ui.dbg_regs);
      mem::dbg_fp_regs_read(ui.dbg_fp);
      ui.toast(mem::bp_status());
    }
    ImGui::SameLine();
    if (chip_button("暂停", theme::warn)) {
      mem::dbg_pause();
      mem::dbg_regs_read(ui.dbg_regs);
      ui.toast(mem::bp_status());
    }
    ImGui::SameLine();
    if (chip_button("继续", theme::success)) {
      mem::dbg_resume();
      ui.toast(mem::bp_status());
    }
    if (chip_button("步入", theme::peach)) {
      mem::dbg_step();
      mem::dbg_regs_read(ui.dbg_regs);
    }
    ImGui::SameLine();
    if (chip_button("步过", theme::peach)) {
      mem::dbg_step_over();
      ui.toast(mem::bp_status());
    }
    ImGui::SameLine();
    if (chip_button("步出", theme::peach)) {
      mem::dbg_step_out();
      ui.toast(mem::bp_status());
    }
    ImGui::TextDisabled("%s · tid=%d · %s", mem::bp_status(), mem::dbg_get_tid(),
                       mem::dbg_is_paused() ? "已暂停" : "运行中");
    ImGui::Checkbox("命中自动弹寄存器", &ui.auto_regs_on_hit);
    if (ui.dbg_regs.valid) {
      ImGui::Text("PC=%llX  SP=%llX  LR=%llX",
                  (unsigned long long)ui.dbg_regs.pc,
                  (unsigned long long)ui.dbg_regs.sp,
                  (unsigned long long)ui.dbg_regs.x[30]);
      // 编辑寄存器
      ImGui::SetNextItemWidth(80);
      ImGui::InputText("名", ui.dbg_reg_name, sizeof(ui.dbg_reg_name));
      ImGui::SameLine();
      ImGui::SetNextItemWidth(120);
      ImGui::InputText("值", ui.dbg_reg_val, sizeof(ui.dbg_reg_val));
      ImGui::SameLine();
      if (chip_button("写入寄存器", theme::danger)) {
        char* end = nullptr;
        uint64_t v = std::strtoull(ui.dbg_reg_val, &end, 0);
        if (mem::dbg_reg_set(ui.dbg_reg_name, v)) {
          mem::dbg_regs_read(ui.dbg_regs);
          ui.toast("寄存器已写");
        } else
          ui.toast(mem::bp_status());
      }
      if (ImGui::BeginChild("regs", ImVec2(0, 160), ImGuiChildFlags_Borders)) {
        for (int i = 0; i < 31; ++i) {
          ImGui::Text("x%-2d  %016llX", i,
                      (unsigned long long)ui.dbg_regs.x[i]);
          if ((i % 2) == 0) ImGui::SameLine(ImGui::GetWindowWidth() * 0.5f);
        }
      }
      ImGui::EndChild();
      if (ui.dbg_fp.valid) {
        ImGui::TextColored(theme::muted, "浮点 d0–d15 (NEON 低 64 位)");
        if (ImGui::BeginChild("fpregs", ImVec2(0, 120), ImGuiChildFlags_Borders)) {
          for (int i = 0; i < 16; ++i) {
            ImGui::Text("d%-2d  %g  v.hi=%016llX", i, ui.dbg_fp.d[i],
                        (unsigned long long)ui.dbg_fp.v[i][1]);
          }
        }
        ImGui::EndChild();
      } else if (chip_button("读浮点寄存器", theme::secondary)) {
        mem::dbg_fp_regs_read(ui.dbg_fp);
      }
    } else {
      ImGui::TextDisabled("点「刷新」（需 ptrace）");
    }
  } else if (w.kind == Ui::FloatKind::Threads) {
    if (chip_button("刷新线程", theme::accent)) {
      ui.threads = mem::list_threads();
      char b[48];
      std::snprintf(b, sizeof(b), "线程 %d", (int)ui.threads.size());
      ui.toast(b);
    }
    if (ImGui::BeginChild("th", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
      for (auto& t : ui.threads) {
        ImGui::Text("%d  [%s]  %s", t.tid, t.state, t.name);
      }
    }
    ImGui::EndChild();
  } else if (w.kind == Ui::FloatKind::Modules) {
    if (chip_button("刷新模块", theme::accent)) {
      ui.modules = mem::list_modules(
          ui.module_filter[0] ? ui.module_filter : nullptr);
      ui.toast("模块已刷新");
    }
    if (ImGui::BeginChild("mod", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
      for (size_t i = 0; i < ui.modules.size(); ++i) {
        auto& m = ui.modules[i];
        ImGui::PushID((int)i);
        ImGui::Text("0x%llX-0x%llX %s", (unsigned long long)m.start,
                    (unsigned long long)m.end, m.perms);
        ImGui::TextDisabled("%s", m.path.c_str());
        if (chip_button("反汇编", theme::peach))
          open_analysis(ui, m.start, false);
        ImGui::SameLine();
        if (chip_button("浏览", theme::secondary)) {
          ui.browse_base = m.start;
          std::snprintf(ui.edit_addr, sizeof(ui.edit_addr), "0x%llX",
                        (unsigned long long)m.start);
          ui.tab = Tab::Addr; ui.addr_sub = 1;
        }
        ImGui::Separator();
        ImGui::PopID();
      }
    }
    ImGui::EndChild();
  } else if (w.kind == Ui::FloatKind::StackTrace) {
    if (chip_button("抓取调用栈", theme::accent)) {
      ui.stack_frames = mem::dbg_stack_trace(24);
      ui.toast(ui.stack_frames.empty() ? mem::bp_status() : "调用栈已更新");
    }
    if (ImGui::BeginChild("stk", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
      for (size_t i = 0; i < ui.stack_frames.size(); ++i) {
        uintptr_t a = ui.stack_frames[i];
        std::string mod = mem::module_of(a);
        ImGui::PushID((int)i);
        ImGui::Text("#%zu  0x%llX", i, (unsigned long long)a);
        if (!mod.empty()) {
          ImGui::SameLine();
          ImGui::TextDisabled("%s", mod.c_str());
        }
        if (chip_button("反汇编", theme::peach)) open_analysis(ui, a, true);
        ImGui::PopID();
      }
    }
    ImGui::EndChild();
  }
}

void draw_all_float_windows(Ui& ui, float dt) {
  (void)dt;
  // 清理已关窗口
  ui.float_wins.erase(
      std::remove_if(ui.float_wins.begin(), ui.float_wins.end(),
                     [](const Ui::FloatWin& w) { return !w.open; }),
      ui.float_wins.end());

  for (auto& w : ui.float_wins) {
    if (!w.open) continue;
    update_float_win_geom(w);

    ImGui::SetNextWindowPos(w.pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(w.size, ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(220.f, 160.f),
                                        ImVec2(9999.f, 9999.f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          ImVec4(1.f, 0.96f, 0.97f, 0.99f));
    // 标题栏加深一点 + 深色字，避免「标题看不见」
    ImGui::PushStyleColor(ImGuiCol_TitleBg,
                          ImVec4(1.f, 0.70f, 0.82f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,
                          ImVec4(1.f, 0.58f, 0.74f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed,
                          ImVec4(1.f, 0.78f, 0.86f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text);
    ImGui::PushStyleColor(ImGuiCol_Button, theme::bubble);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::danger);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 18.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.f, 8.f));

    char wid[64];
    std::snprintf(wid, sizeof(wid), "%s###fw%d", w.title, w.id);
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    if (ImGui::Begin(wid, &w.open, flags)) {
      // 尺寸由 update_float_win_geom 维护，不回写避免内容撑高
      ImGui::TextDisabled("拖标题 · 右边/底边/角缩放");
      float body_h = ImGui::GetContentRegionAvail().y - 18.f;
      if (body_h < 40.f) body_h = 40.f;
      if (ImGui::BeginChild(
              "fw_body", ImVec2(0, body_h), ImGuiChildFlags_None,
              ImGuiWindowFlags_AlwaysVerticalScrollbar |
                  ImGuiWindowFlags_HorizontalScrollbar)) {
        draw_float_win_content(ui, w);
      }
      ImGui::EndChild();

      ImDrawList* dl = ImGui::GetWindowDrawList();
      ImVec2 br = ImGui::GetWindowPos();
      ImVec2 sz = ImGui::GetWindowSize();
      // 底边/右边指示
      dl->AddRectFilled(ImVec2(br.x + 12.f, br.y + sz.y - 5.f),
                        ImVec2(br.x + sz.x - 28.f, br.y + sz.y - 2.f),
                        IM_COL32(255, 160, 190, 140), 2.f);
      dl->AddRectFilled(ImVec2(br.x + sz.x - 5.f, br.y + 28.f),
                        ImVec2(br.x + sz.x - 2.f, br.y + sz.y - 28.f),
                        IM_COL32(255, 160, 190, 140), 2.f);
      ImVec2 a(br.x + sz.x - 22.f, br.y + sz.y - 22.f);
      ImVec2 b(br.x + sz.x - 3.f, br.y + sz.y - 3.f);
      dl->AddRectFilled(a, b, IM_COL32(255, 160, 190, 220), 8.f);
      dl->AddTriangleFilled(ImVec2(b.x - 2, a.y + 4), b,
                            ImVec2(a.x + 4, b.y - 2),
                            IM_COL32(255, 240, 245, 255));
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(8);
  }
}

/** 根据字段语义选择默认键盘页 */
soft_ime::Mode ime_mode_from_hint(const char* mode) {
  if (!mode) return soft_ime::Mode::Abc;
  if (std::strcmp(mode, "hex") == 0 || std::strcmp(mode, "number") == 0 ||
      std::strcmp(mode, "addr") == 0)
    return soft_ime::Mode::Hex;
  if (std::strcmp(mode, "asm") == 0)
    return soft_ime::Mode::Asm;
  return soft_ime::Mode::Abc;
}

/** ImGui 输入框：单击弹出内嵌软键盘，内容实时写进 buf */
void draw_ime_field(Ui& ui, const char* id, const char* label, char* buf, size_t cap,
                    const char* title, const char* mode, float /*dt*/) {
  if (!buf || cap < 2) return;
  ImGui::PushID(id);
  if (label && label[0])
    ImGui::TextColored(theme::muted, "%s", label);

  const bool active = (ui.ime_target == buf) && soft_ime::is_open();

  if (active) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.f, 0.9f, 0.93f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 0.55f, 0.72f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.f);
  }

  ImGui::SetNextItemWidth(-1.f);
  ImGui::InputText("##field", buf, cap, ImGuiInputTextFlags_ReadOnly);
  bool clicked = ImGui::IsItemClicked();
  bool hovered = ImGui::IsItemHovered();

  if (active) {
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    ImGui::TextColored(theme::accent, "键盘");
  } else if (hovered) {
    ImGui::SameLine();
    ImGui::TextDisabled("点击输入");
  }

  if (clicked) {
    ui.ime_target = buf;
    soft_ime::Mode m = ime_mode_from_hint(mode);
    if (!soft_ime::open(buf, cap, title ? title : (label ? label : "输入"), m)) {
      ui.toast("键盘打开失败");
      ui.ime_target = nullptr;
    } else {
      ui.toast("软键盘已开 · HEX / ABC / ASM");
    }
  }

  ImGui::PopID();
}

// ── 悬浮球（呼吸 + 旋转环 + 按压缩放）────────────────────
void draw_float_ball(Ui& ui, float dt) {
  ui.ball_time += dt;
  ui.ball_spin += dt * (ui.want_open ? 2.2f : 1.1f);

  const float breath = 0.5f + 0.5f * std::sin(ui.ball_time * 2.4f);
  const float base_r = ui.ball_r;
  const float r =
      base_r * (1.f + breath * 0.06f - ui.ball_press * 0.12f);
  const float d = r * 2.2f;  // 窗口稍大，容纳光晕

  ImGui::SetNextWindowPos(ui.ball_pos,
                          ui.ball_pos_init ? ImGuiCond_Always : ImGuiCond_Once);
  ImGui::SetNextWindowSize(ImVec2(d, d));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, d * 0.5f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));

  ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground;

  if (ImGui::Begin("##float_ball", nullptr, flags)) {
    ui.ball_pos_init = true;
    ImVec2 wp = ImGui::GetWindowPos();
    ImVec2 ws = ImGui::GetWindowSize();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(wp.x + ws.x * 0.5f, wp.y + ws.y * 0.5f);

    // 粉嫩光晕
    const float glow_r = r + 6.f + breath * 10.f;
    dl->AddCircleFilled(c, glow_r,
                        col4(1.f, 0.55f, 0.75f, 0.18f + breath * 0.12f), 48);

    // 旋转虚线环（草莓粉）
    const int segs = 36;
    for (int i = 0; i < segs; i += 2) {
      float a0 = ui.ball_spin + (float)i / segs * 6.2831853f;
      float a1 = ui.ball_spin + (float)(i + 1) / segs * 6.2831853f;
      ImVec2 p0(c.x + std::cos(a0) * (r + 3.f),
                c.y + std::sin(a0) * (r + 3.f));
      ImVec2 p1(c.x + std::cos(a1) * (r + 3.f),
                c.y + std::sin(a1) * (r + 3.f));
      dl->AddLine(p0, p1, col4(1.f, 0.65f, 0.8f, 0.6f + breath * 0.3f), 2.5f);
    }

    // 球体：草莓冰淇淋
    const float open_t = ui.open_anim;
    ImU32 c_inner = col4(lerpf(1.f, 1.f, open_t), lerpf(0.7f, 0.55f, open_t),
                         lerpf(0.8f, 0.7f, open_t), 0.97f);
    ImU32 c_outer = col4(lerpf(1.f, 0.95f, open_t), lerpf(0.55f, 0.45f, open_t),
                         lerpf(0.7f, 0.65f, open_t), 0.97f);
    dl->AddCircleFilled(c, r - 1.f, c_outer, 48);
    dl->AddCircleFilled(c, r * 0.72f, c_inner, 48);
    dl->AddCircle(c, r - 1.f, col4(1, 1, 1, 0.55f + breath * 0.25f), 48, 2.f);
    // 小球心装饰
    draw_heart(dl, ImVec2(c.x, c.y + 2.f), r * 0.22f,
               col4(1.f, 0.4f, 0.6f, 0.85f));

    // 打开时画小 X，关闭时靠爱心
    if (open_t > 0.5f) {
      float s = r * 0.26f;
      dl->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s),
                  IM_COL32(255, 255, 255, 250), 3.8f);
      dl->AddLine(ImVec2(c.x + s, c.y - s), ImVec2(c.x - s, c.y + s),
                  IM_COL32(255, 255, 255, 250), 3.8f);
    }

    ImGui::SetCursorScreenPos(wp);
    ImGui::InvisibleButton("ball_hit", ws);
    bool hovered = ImGui::IsItemHovered();
    bool held = ImGui::IsItemActive();
    ui.ball_press =
        lerpf(ui.ball_press, held ? 1.f : (hovered ? 0.2f : 0.f),
              clampf(dt * 14.f, 0.f, 1.f));

    if (held && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 8.f)) {
      ImVec2 delta = ImGui::GetIO().MouseDelta;
      ui.ball_pos.x += delta.x;
      ui.ball_pos.y += delta.y;
    }

    // 单击开/关面板（已取消长按退出，仅工具页「退出程序」按钮退出）
    if (ImGui::IsItemDeactivated() &&
        !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 8.f) && !ui.want_exit) {
      ui.want_open = !ui.want_open;
      ui.toast(ui.want_open ? "打开面板" : "面板已收起");
    }

    auto disp = vkeng::display();
    ui.ball_pos.x = clampf(ui.ball_pos.x, 0.f, (float)disp.width - d);
    ui.ball_pos.y = clampf(ui.ball_pos.y, 0.f, (float)disp.height - d);
  }
  ImGui::End();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(2);
}


// ── 页内子导航芯片 ───────────────────────────────────────
bool draw_sub_nav(Ui& ui, int& sub, const char* const* names, int n, float dt,
                  int& bi) {
  bool changed = false;
  float avail = ImGui::GetContentRegionAvail().x;
  float gap = 5.f;
  int cols = n;
  if (avail < 300.f && n > 4) cols = (n + 1) / 2;
  if (cols < 1) cols = 1;
  float bw = (avail - gap * (float)(cols - 1)) / (float)cols;
  if (bw < 52.f) bw = 52.f;
  for (int i = 0; i < n; ++i) {
    if (i % cols) ImGui::SameLine(0, gap);
    ImVec4 c = (sub == i) ? theme::accent : theme::tab_idle;
    char id[48];
    std::snprintf(id, sizeof(id), "sn%d_%s", i, names[i]);
    if (bi >= 120) bi = 40;  // 防溢出 wrap
    if (anim_button(id, names[i], ImVec2(bw, theme::btn_min_h * 0.82f),
                    ui.btns[bi++], dt, c)) {
      if (sub != i) {
        sub = i;
        changed = true;
      }
    }
  }
  ImGui::Spacing();
  return changed;
}

void tab_search(Ui& ui, float dt, int& bi);
void tab_results(Ui& ui, float dt, int& bi);
void tab_memory(Ui& ui, float dt, int& bi);
void tab_addrlist(Ui& ui, float dt, int& bi);
void tab_pointer(Ui& ui, float dt, int& bi);
void tab_struct(Ui& ui, float dt, int& bi);
void tab_tools(Ui& ui, float dt, int& bi);
void tab_auto(Ui& ui, float dt, int& bi);

void tab_scan(Ui& ui, float dt, int& bi) {
  static const char* k[] = {"搜索条件", "扫描结果"};
  draw_sub_nav(ui, ui.scan_sub, k, 2, dt, bi);
  if (ui.scan_sub == 0)
    tab_search(ui, dt, bi);
  else
    tab_results(ui, dt, bi);
}

void tab_addr(Ui& ui, float dt, int& bi) {
  static const char* k[] = {"地址表", "Hex浏览"};
  draw_sub_nav(ui, ui.addr_sub, k, 2, dt, bi);
  if (ui.addr_sub == 0)
    tab_addrlist(ui, dt, bi);
  else
    tab_memory(ui, dt, bi);
}

void tab_analyze(Ui& ui, float dt, int& bi) {
  static const char* k[] = {"指针扫描", "结构体"};
  draw_sub_nav(ui, ui.analyze_sub, k, 2, dt, bi);
  if (ui.analyze_sub == 0)
    tab_pointer(ui, dt, bi);
  else
    tab_struct(ui, dt, bi);
}

void tab_debug(Ui& ui, float dt, int& bi) {
  static const char* k[] = {"控制", "断点", "补丁", "注入", "地图"};
  draw_sub_nav(ui, ui.debug_sub, k, 5, dt, bi);
  // 实现见下方 tab_tools 重构：直接调用并传入 sub
  tab_tools(ui, dt, bi);
}


// ── 各 Tab 内容 ───────────────────────────────────────────
void tab_search(Ui& ui, float dt, int& bi) {
  if (!mem::is_attached()) {
    ImGui::TextColored(theme::warn, "未附加进程 · 请到「进程」页操作");
  } else {
    ImGui::TextColored(theme::success, "目标: %d %s", mem::attached_pid(),
                       mem::attached_name());
  }

  ImGui::TextColored(theme::muted, "数值 / 字符串类型");
  {
    // 两行：数值一行，Hex/字符串一行，避免挤成看不见
    const int n = (int)ValueType::COUNT;
    const float gap = 4.f;
    auto row_types = [&](int from, int to) {
      int cnt = to - from;
      if (cnt <= 0) return;
      float bw =
          (ImGui::GetContentRegionAvail().x - gap * (float)(cnt - 1)) /
          (float)cnt;
      for (int i = from; i < to; ++i) {
        if (i > from) ImGui::SameLine(0, gap);
        bool sel = (int)ui.vtype == i;
        ImVec4 c = sel ? theme::accent : theme::idle;
        if (anim_button(kValueTypeNames[i], kValueTypeNames[i], ImVec2(bw, 0),
                        ui.btns[bi++], dt, c)) {
          ui.vtype = (ValueType)i;
          ui.toast("类型已切换");
        }
      }
    };
    row_types(0, (int)ValueType::Hex);          // I8..Double
    row_types((int)ValueType::Hex, n);          // Hex UTF8 U16LE
  }

  ImGui::Spacing();
  ImGui::TextColored(theme::muted, "搜索条件");
  if (ImGui::BeginCombo("##smode", kSearchModeNames[(int)ui.smode])) {
    for (int i = 0; i < (int)SearchMode::COUNT; ++i) {
      bool sel = (int)ui.smode == i;
      if (ImGui::Selectable(kSearchModeNames[i], sel)) ui.smode = (SearchMode)i;
      if (sel) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }

  ImGui::TextColored(theme::muted, "内存范围（CE 风格）");
  if (ImGui::BeginCombo("##rgn", mem::region_filter_name(ui.region_filter))) {
    for (int i = 0; i < (int)mem::RegionFilter::COUNT; ++i) {
      auto rf = (mem::RegionFilter)i;
      bool sel = ui.region_filter == rf;
      if (ImGui::Selectable(mem::region_filter_name(rf), sel))
        ui.region_filter = rf;
      if (sel) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }

  ImGui::Spacing();
  const bool is_str =
      ui.vtype == ValueType::StrUtf8 || ui.vtype == ValueType::StrUtf16;
  const char* vhint = ui.vtype == ValueType::Hex
                          ? "hex"
                          : (is_str ? "text" : "number");
  const char* vlabel =
      ui.vtype == ValueType::Hex
          ? "Hex/特征码 (支持 ?? 通配)"
          : (is_str ? "搜索字符串（UTF-8 输入）" : "搜索数值");
  draw_ime_field(ui, "val", vlabel, ui.value_buf, sizeof(ui.value_buf),
                 "搜索内容", vhint, dt);
  if (ui.smode == SearchMode::Between && !is_str) {
    ImGui::Spacing();
    draw_ime_field(ui, "val2", "上限数值", ui.value_buf2, sizeof(ui.value_buf2),
                   "上限数值", "number", dt);
  } else if (ui.smode == SearchMode::Fuzzy && !is_str) {
    ImGui::Spacing();
    draw_ime_field(ui, "val2", "容差 (±)", ui.value_buf2, sizeof(ui.value_buf2),
                   "模糊容差", "number", dt);
    ImGui::TextDisabled("模糊：|当前值 - 目标| ≤ 容差");
  }
  if (ui.vtype == ValueType::Hex)
    ImGui::TextDisabled("通配: ?? 任意字节 · A? / ?B 半字节");
  if (is_str) {
    ImGui::Checkbox("忽略大小写 (ASCII)", &ui.str_ignore_case);
    ImGui::TextDisabled(
        ui.vtype == ValueType::StrUtf8
            ? "UTF-8 按字节匹配 · 适合 Java/Native 字符串"
            : "UTF-16LE · 适合 Java/C# 宽字符串");
  }

  auto fill_scan_cfg = [&](mem::ScanConfig& cfg, char* err, size_t err_cap) {
    // 字符串：把 ignore case 传给 v2
    const char* v2 = ui.value_buf2;
    char ci_buf[8] = "i";
    if (is_str) v2 = ui.str_ignore_case ? ci_buf : "";
    return mem::parse_scan_values(to_mem_type(ui.vtype), to_mem_mode(ui.smode),
                                  ui.value_buf, v2, cfg, err, err_cap);
  };

  bool can_scan = mem::is_attached() && !mem::scan_busy();
  ImGui::Spacing();
  float half = (ImGui::GetContentRegionAvail().x - 8.f) * 0.5f;
  if (anim_button("first", "首次搜索", ImVec2(half, 0), ui.btns[bi++], dt,
                  theme::accent, can_scan)) {
    mem::ScanConfig cfg;
    char err[96]{};
    if (!fill_scan_cfg(cfg, err, sizeof(err))) {
      ui.toast(err[0] ? err : "参数错误");
    } else {
      cfg.region = ui.region_filter;
      ui.result_page = 0;
      if (!mem::start_first_scan(cfg)) {
        ui.toast(mem::scan_status());
      } else {
        ui.searching = true;
        ui.toast(is_str ? "字符串搜索已开始…" : "首次搜索已开始…");
      }
    }
  }
  ImGui::SameLine();
  if (anim_button("refine", "再次搜索", ImVec2(half, 0), ui.btns[bi++], dt,
                  theme::accent2, can_scan && ui.search_round > 0)) {
    mem::ScanConfig cfg;
    char err[96]{};
    if (!fill_scan_cfg(cfg, err, sizeof(err))) {
      ui.toast(err[0] ? err : "参数错误");
    } else {
      cfg.region = ui.region_filter;
      if (!mem::start_next_scan(cfg)) {
        ui.toast(mem::scan_status());
      } else {
        ui.searching = true;
        ui.toast("再次筛选中…");
      }
    }
  }

  ImGui::Spacing();
  if (anim_button("clear", "清空搜索", ImVec2(-1, 0), ui.btns[bi++], dt,
                  theme::danger, ui.search_round > 0 || !ui.results.empty())) {
    mem::clear_scan();
    ui.results.clear();
    ui.result_count = 0;
    ui.search_round = 0;
    ui.selected_result = -1;
    ui.result_page = 0;
    ui.toast("已清空搜索结果");
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Text("轮次: %d   结果: %d", ui.search_round, ui.result_count);
  if (mem::scan_busy() || ui.searching) {
    ui.search_spin += dt * 8.f;
    ImGui::TextColored(theme::warn, "搜索中 %.0f%% %s",
                       mem::scan_progress() * 100.f,
                       (ui.search_spin - (int)ui.search_spin) > 0.5f ? "..."
                                                                     : "..");
  }
}

void tab_results(Ui& ui, float dt, int& bi) {
  if (ui.result_count <= 0 && !ui.searching) {
    ImGui::TextColored(theme::warn, "暂无结果 · 到「搜索」页执行首次搜索");
    ImGui::TextDisabled("支持精确/区间/变化/模糊 · UTF8/Hex/数值");
  }
  int pages =
      ui.result_count > 0
          ? (ui.result_count + ui.result_page_size - 1) / ui.result_page_size
          : 1;
  int from = ui.result_count ? ui.result_page * ui.result_page_size + 1 : 0;
  int to = std::min(ui.result_count,
                    (ui.result_page + 1) * ui.result_page_size);
  ImGui::TextColored(theme::muted,
                     "共 %d 条 · 第 %d/%d 页 · 显示 %d–%d",
                     ui.result_count, ui.result_page + 1, pages, from, to);
  draw_ime_field(ui, "rfilt", "结果模糊过滤", ui.result_filter,
                 sizeof(ui.result_filter), "结果过滤", "text", dt);
  ImGui::Spacing();

  // 分页控制
  float half = (ImGui::GetContentRegionAvail().x - 8.f) * 0.5f;
  float q = (ImGui::GetContentRegionAvail().x - 12.f) / 3.f;
  if (anim_button("pg_prev", "上一页", ImVec2(q, 0), ui.btns[bi++], dt,
                  theme::secondary, ui.result_page > 0)) {
    ui.result_page--;
    sync_results_from_engine(ui);
  }
  ImGui::SameLine();
  if (anim_button("pg_next", "下一页", ImVec2(q, 0), ui.btns[bi++], dt,
                  theme::secondary,
                  ui.result_page + 1 < pages && ui.result_count > 0)) {
    ui.result_page++;
    sync_results_from_engine(ui);
  }
  ImGui::SameLine();
  // 页大小切换
  if (anim_button("pg_sz",
                  ui.result_page_size == 50
                      ? "每页50"
                      : (ui.result_page_size == 100 ? "每页100" : "每页200"),
                  ImVec2(q, 0), ui.btns[bi++], dt, theme::idle)) {
    if (ui.result_page_size == 50)
      ui.result_page_size = 100;
    else if (ui.result_page_size == 100)
      ui.result_page_size = 200;
    else
      ui.result_page_size = 50;
    ui.result_page = 0;
    sync_results_from_engine(ui);
  }

  if (anim_button("sel_all", "冻结本页", ImVec2(half, 0), ui.btns[bi++], dt,
                  theme::warn, !ui.results.empty() && mem::is_attached())) {
    size_t sz = mem::type_size_of(to_mem_type(ui.vtype));
    if (mem::is_pattern_type(to_mem_type(ui.vtype))) sz = 1;
    for (auto& r : ui.results) {
      r.frozen = true;
      mem::set_frozen(r.addr, true, r.bits, sz);
    }
    ui.toast("已冻结当前页地址");
  }
  ImGui::SameLine();
  if (anim_button("unfreeze", "取消冻结", ImVec2(half, 0), ui.btns[bi++], dt,
                  theme::secondary, !ui.results.empty())) {
    mem::clear_all_frozen();
    for (auto& r : ui.results) r.frozen = false;
    ui.toast("已取消全部冻结");
  }

  if (anim_button("refresh_vals", "刷新当前页", ImVec2(-1, 0), ui.btns[bi++],
                  dt, theme::accent, mem::is_attached() && !ui.results.empty())) {
    sync_results_from_engine(ui);
    ui.toast("已刷新显示值");
  }

  // 断点状态
  ImGui::TextDisabled("%s", mem::bp_status());
  if (mem::bp_ptrace_active() && !mem::bp_is_stopped()) {
    if (chip_button("停止等待", theme::warn)) {
      /* 仅提示：下次命中前可 clear */
      ui.toast(mem::bp_status());
    }
  }

  ImGui::Spacing();
  if (ImGui::BeginChild("results_scroll", ImVec2(0, -8),
                        ImGuiChildFlags_Borders,
                        ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
    if (ui.results.empty()) {
      ImGui::Spacing();
      ImGui::TextDisabled("暂无结果 · 请先到「搜索」执行首次搜索");
    }
    for (int i = 0; i < (int)ui.results.size(); ++i) {
      auto& r = ui.results[i];
      if (ui.result_filter[0]) {
        char line[96];
        std::snprintf(line, sizeof(line), "%llX %s",
                      (unsigned long long)r.addr, r.value);
        if (!mem::fuzzy_match(line, ui.result_filter)) continue;
      }
      ImGui::PushID(i);
      bool sel = ui.selected_result == i;
      if (sel) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
                              ImVec4(0.15f, 0.35f, 0.55f, 0.35f));
      }
      // 多行操作：增高，chip 按钮保证中文可见
      if (ImGui::BeginChild("row", ImVec2(0, theme::chip_h * 3.2f + 16.f),
                            ImGuiChildFlags_Borders)) {
        ImGui::Text("0x%llX", (unsigned long long)r.addr);
        ImGui::SameLine();
        ImGui::TextColored(theme::accent, " = %s", r.value);
        if (r.frozen) {
          ImGui::SameLine();
          ImGui::TextColored(theme::warn, "[冻结]");
        }
        if (chip_button("选中", theme::bubble)) {
          ui.selected_result = i;
          std::snprintf(ui.edit_addr, sizeof(ui.edit_addr), "0x%llX",
                        (unsigned long long)r.addr);
          std::snprintf(ui.edit_value, sizeof(ui.edit_value), "%s", r.value);
        }
        ImGui::SameLine();
        if (chip_button(r.frozen ? "解冻" : "冻结",
                        r.frozen ? theme::warn : theme::teal)) {
          size_t sz = mem::type_size_of(to_mem_type(ui.vtype));
          r.frozen = !r.frozen;
          mem::set_frozen(r.addr, r.frozen, r.bits, sz);
          ui.toast(r.frozen ? "地址已冻结" : "地址已解冻");
        }
        ImGui::SameLine();
        if (chip_button("编辑", theme::secondary)) {
          ui.selected_result = i;
          std::snprintf(ui.edit_addr, sizeof(ui.edit_addr), "0x%llX",
                        (unsigned long long)r.addr);
          std::snprintf(ui.edit_value, sizeof(ui.edit_value), "%s", r.value);
          ui.browse_base = r.addr;
          ui.tab = Tab::Addr; ui.addr_sub = 1;
          ui.tab_anim = 0.f;
        }
        ImGui::SameLine();
        if (chip_button("加入表", theme::accent2)) {
          ui.addr_table.add(r.addr, to_mem_type(ui.vtype), "扫描");
          ui.toast("已加入地址表");
        }
        // 第二行：反汇编 / 硬件断点
        if (chip_button("反汇编", theme::peach)) {
          open_analysis(ui, r.addr, false);
        }
        ImGui::SameLine();
        if (chip_button("函数汇编", theme::peach)) {
          uintptr_t code = r.addr;
          uint64_t ptr = 0;
          if (mem::read_mem(r.addr, &ptr, sizeof(ptr)) && ptr > 0x1000)
            code = (uintptr_t)ptr;
          open_analysis(ui, code, true);
        }
        ImGui::SameLine();
        if (chip_button("写断点", theme::danger)) {
          int sz = (int)mem::type_size_of(to_mem_type(ui.vtype));
          if (sz > 8) sz = 8;
          int id = mem::bp_set(r.addr, mem::BpType::WatchW, sz);
          if (id >= 0) {
            mem::bp_arm_and_continue();
            char buf[96];
            std::snprintf(buf, sizeof(buf), "perf写断点#%d 已下", id);
            ui.toast(buf);
          } else
            ui.toast(mem::bp_status());
        }
        ImGui::SameLine();
        if (chip_button("读写断点", theme::danger)) {
          int sz = (int)mem::type_size_of(to_mem_type(ui.vtype));
          if (sz > 8) sz = 8;
          int id = mem::bp_set(r.addr, mem::BpType::WatchRW, sz);
          if (id >= 0) {
            mem::bp_arm_and_continue();
            char buf[96];
            std::snprintf(buf, sizeof(buf), "perf读写断点#%d", id);
            ui.toast(buf);
          } else
            ui.toast(mem::bp_status());
        }
        ImGui::SameLine();
        if (chip_button("执行断点", theme::warn)) {
          uintptr_t code = r.addr;
          uint64_t ptr = 0;
          if (mem::read_mem(r.addr, &ptr, sizeof(ptr)) && ptr > 0x10000)
            code = (uintptr_t)ptr;
          int id = mem::bp_set(code & ~3ull, mem::BpType::Exec, 4);
          if (id >= 0) {
            mem::bp_arm_and_continue();
            char buf[96];
            std::snprintf(buf, sizeof(buf), "执行断点#%d @0x%llX", id,
                          (unsigned long long)code);
            ui.toast(buf);
          } else
            ui.toast(mem::bp_status());
        }
        ImGui::SameLine();
        if (chip_button("新窗口", theme::secondary))
          open_analysis(ui, r.addr, false);
      }
      ImGui::EndChild();
      if (sel) ImGui::PopStyleColor();
      ImGui::PopID();
    }
  }
  ImGui::EndChild();
}

void do_read_editor(Ui& ui) {
  if (!mem::is_attached()) {
    ui.toast("未附加进程");
    return;
  }
  uintptr_t addr = 0;
  if (!mem::parse_addr(ui.edit_addr, addr)) {
    ui.toast("地址格式错误");
    return;
  }
  uint8_t buf[16]{};
  if (!mem::read_mem(addr, buf, 16)) {
    ui.toast("读取失败");
    return;
  }
  for (int i = 0; i < 16; ++i)
    std::snprintf(ui.hex_preview[i], 3, "%02X", buf[i]);
  for (int i = 0; i < 16; ++i) {
    char c = (char)buf[i];
    ui.ascii_preview[i] = (c >= 32 && c < 127) ? c : '.';
  }
  ui.ascii_preview[16] = 0;

  uint64_t bits = 0;
  size_t sz = 0;
  // 用当前类型解释前 N 字节
  auto vt = to_mem_type(ui.vtype);
  sz = mem::type_size_of(vt);
  if (sz > 8) sz = 8;
  std::memcpy(&bits, buf, sz);
  mem::format_value(vt, bits, ui.edit_value, sizeof(ui.edit_value));
  ui.toast("读取成功");
}

void do_write_editor(Ui& ui, int delta) {
  if (!mem::is_attached()) {
    ui.toast("未附加进程");
    return;
  }
  uintptr_t addr = 0;
  if (!mem::parse_addr(ui.edit_addr, addr)) {
    ui.toast("地址格式错误");
    return;
  }
  auto vt = to_mem_type(ui.vtype);
  uint64_t bits = 0;
  size_t sz = 0;
  if (delta != 0) {
    // 先读再加减
    uint8_t tmp[8]{};
    sz = mem::type_size_of(vt);
    if (sz > 8) sz = 8;
    if (!mem::read_mem(addr, tmp, sz)) {
      ui.toast("读取失败，无法加减");
      return;
    }
    std::memcpy(&bits, tmp, sz);
    if (vt == mem::ValType::F32) {
      float f;
      uint32_t u = (uint32_t)bits;
      std::memcpy(&f, &u, 4);
      f += (float)delta;
      std::memcpy(&u, &f, 4);
      bits = u;
    } else if (vt == mem::ValType::F64) {
      double d;
      std::memcpy(&d, &bits, 8);
      d += (double)delta;
      std::memcpy(&bits, &d, 8);
    } else {
      bits = (uint64_t)((int64_t)bits + delta);
    }
  } else {
    if (!mem::parse_value(vt, ui.edit_value, bits, sz)) {
      ui.toast("数值解析失败");
      return;
    }
  }
  if (!mem::write_mem(addr, &bits, sz)) {
    ui.toast("写入失败（权限/地址）");
    return;
  }
  mem::format_value(vt, bits, ui.edit_value, sizeof(ui.edit_value));
  // 刷新 hex 预览（不覆盖 toast）
  uint8_t buf[16]{};
  if (mem::read_mem(addr, buf, 16)) {
    for (int i = 0; i < 16; ++i)
      std::snprintf(ui.hex_preview[i], 3, "%02X", buf[i]);
    for (int i = 0; i < 16; ++i) {
      char c = (char)buf[i];
      ui.ascii_preview[i] = (c >= 32 && c < 127) ? c : '.';
    }
    ui.ascii_preview[16] = 0;
  }
  if (delta > 0) ui.toast("已 +1 并写入");
  else if (delta < 0) ui.toast("已 -1 并写入");
  else ui.toast("写入成功");
}

void browse_reload(Ui& ui) {
  ui.browse_valid = false;
  if (!mem::is_attached()) return;
  uintptr_t a = ui.browse_base;
  if (a == 0 && !mem::parse_addr(ui.edit_addr, a)) return;
  if (a) ui.browse_base = a;
  size_t n = (size_t)ui.browse_rows * 16;
  if (n > sizeof(ui.browse_bytes)) n = sizeof(ui.browse_bytes);
  if (mem::read_mem(ui.browse_base, ui.browse_bytes, n)) {
    ui.browse_valid = true;
    std::snprintf(ui.edit_addr, sizeof(ui.edit_addr), "0x%llX",
                  (unsigned long long)ui.browse_base);
  }
}

void tab_memory(Ui& ui, float dt, int& bi) {
  ImGui::TextColored(theme::muted, "内存浏览 · CE Memory View");
  if (!mem::is_attached())
    ImGui::TextColored(theme::warn, "未附加进程");

  draw_ime_field(ui, "addr", "地址", ui.edit_addr, sizeof(ui.edit_addr),
                 "内存地址", "hex", dt);
  ImGui::Spacing();
  draw_ime_field(ui, "eval", "数值写入", ui.edit_value, sizeof(ui.edit_value),
                 "写入数值", "number", dt);

  float q = (ImGui::GetContentRegionAvail().x - 18.f) / 4.f;
  if (anim_button("read", "读取", ImVec2(q, 0), ui.btns[bi++], dt, theme::accent,
                  mem::is_attached())) {
    do_read_editor(ui);
    uintptr_t a = 0;
    if (mem::parse_addr(ui.edit_addr, a)) {
      ui.browse_base = a;
      browse_reload(ui);
    }
  }
  ImGui::SameLine();
  if (anim_button("write", "写入", ImVec2(q, 0), ui.btns[bi++], dt,
                  theme::success, mem::is_attached())) {
    do_write_editor(ui, 0);
    browse_reload(ui);
  }
  ImGui::SameLine();
  if (anim_button("addtab", "加入地址表", ImVec2(q, 0), ui.btns[bi++], dt,
                  theme::accent2, mem::is_attached())) {
    uintptr_t a = 0;
    if (mem::parse_addr(ui.edit_addr, a)) {
      ui.addr_table.add(a, to_mem_type(ui.vtype), ui.table_desc);
      ui.toast("已加入地址表");
    }
  }
  ImGui::SameLine();
  if (anim_button("bref", "刷新浏览", ImVec2(q, 0), ui.btns[bi++], dt,
                  theme::secondary, mem::is_attached())) {
    uintptr_t a = 0;
    if (mem::parse_addr(ui.edit_addr, a)) ui.browse_base = a;
    browse_reload(ui);
    ui.toast(ui.browse_valid ? "浏览已刷新" : "读取失败");
  }

  float half = (ImGui::GetContentRegionAvail().x - 8.f) * 0.5f;
  if (anim_button("pgup", "上翻", ImVec2(half, 0), ui.btns[bi++], dt,
                  theme::teal, mem::is_attached())) {
    size_t page = (size_t)ui.browse_rows * 16;
    if (ui.browse_base > page) ui.browse_base -= page;
    else ui.browse_base = 0;
    browse_reload(ui);
  }
  ImGui::SameLine();
  if (anim_button("pgdn", "下翻", ImVec2(half, 0), ui.btns[bi++], dt,
                  theme::peach, mem::is_attached())) {
    ui.browse_base += (uintptr_t)ui.browse_rows * 16;
    browse_reload(ui);
  }

  ImGui::Spacing();
  ImGui::TextColored(theme::muted, "Hex 视图 @ 0x%llX",
                     (unsigned long long)ui.browse_base);
  // 高度随窗口伸缩；预留下方按钮区，避免吃光后看不见操作
  {
    float hv = ImGui::GetContentRegionAvail().y - 260.f;
    if (hv < 120.f) hv = 120.f;
    if (ImGui::BeginChild("hexview", ImVec2(0, hv), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar |
                              ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
      if (!ui.browse_valid) {
        ImGui::TextDisabled("点「刷新浏览」加载内存");
      } else {
        for (int row = 0; row < ui.browse_rows; ++row) {
          uintptr_t ra = ui.browse_base + (uintptr_t)row * 16;
          ImGui::Text("%08llX ", (unsigned long long)ra);
          ImGui::SameLine(0, 4);
          char ascii[17]{};
          for (int col = 0; col < 16; ++col) {
            int idx = row * 16 + col;
            uint8_t b = ui.browse_bytes[idx];
            ImGui::Text("%02X", b);
            if (col != 15) ImGui::SameLine(0, 3);
            ascii[col] = (b >= 32 && b < 127) ? (char)b : '.';
          }
          ImGui::SameLine(0, 8);
          ImGui::TextColored(theme::muted, "%s", ascii);
        }
      }
    }
    ImGui::EndChild();
  }

  ImGui::Spacing();
  if (anim_button("inc", "数值 +1", ImVec2(-1, 0), ui.btns[bi++], dt,
                  theme::success, mem::is_attached()))
    do_write_editor(ui, +1);
  if (anim_button("dec", "数值 -1", ImVec2(-1, 0), ui.btns[bi++], dt,
                  theme::danger, mem::is_attached()))
    do_write_editor(ui, -1);

  ImGui::Separator();
  ImGui::TextColored(theme::muted, "分析");
  if (anim_button("dis_here", "反汇编", ImVec2(-1, 0), ui.btns[bi++], dt,
                  theme::accent2, mem::is_attached())) {
    uintptr_t a = 0;
    if (mem::parse_addr(ui.edit_addr, a)) open_analysis(ui, a, false);
    else ui.toast("地址无效");
  }
  if (anim_button("func_here", "函数汇编 + 伪C", ImVec2(-1, 0), ui.btns[bi++],
                  dt, theme::accent, mem::is_attached())) {
    uintptr_t a = 0;
    if (mem::parse_addr(ui.edit_addr, a)) open_analysis(ui, a, true);
    else ui.toast("地址无效");
  }
  if (anim_button("bp_w", "写观察点", ImVec2(-1, 0), ui.btns[bi++], dt,
                  theme::warn, mem::is_attached())) {
    uintptr_t a = 0;
    if (!mem::parse_addr(ui.edit_addr, a)) ui.toast("地址无效");
    else if (mem::bp_set(a, mem::BpType::WatchW,
                         (int)mem::type_size_of(to_mem_type(ui.vtype))) >= 0) {
      mem::bp_arm_and_continue();
      ui.toast("写观察点已下");
    } else
      ui.toast(mem::bp_status());
  }
}

void tab_addrlist(Ui& ui, float dt, int& bi) {
  ImGui::TextColored(theme::muted, "地址表 · Cheat Table（CE）");
  ImGui::Text("条目: %d", (int)ui.addr_table.size());

  draw_ime_field(ui, "tdesc", "描述", ui.table_desc, sizeof(ui.table_desc),
                 "描述", "text", dt);

  float third = (ImGui::GetContentRegionAvail().x - 12.f) / 3.f;
  if (anim_button("tadd", "添加当前地址", ImVec2(third, 0), ui.btns[bi++], dt,
                  theme::accent, mem::is_attached())) {
    uintptr_t a = 0;
    if (mem::parse_addr(ui.edit_addr, a)) {
      ui.addr_table.add(a, to_mem_type(ui.vtype), ui.table_desc);
      ui.toast("已添加");
    } else
      ui.toast("地址无效");
  }
  ImGui::SameLine();
  if (anim_button("tdel", "删除选中", ImVec2(third, 0), ui.btns[bi++], dt,
                  theme::danger, !ui.addr_table.entries.empty())) {
    ui.addr_table.remove_selected();
    ui.toast("已删除选中");
  }
  ImGui::SameLine();
  if (anim_button("tref", "刷新数值", ImVec2(third, 0), ui.btns[bi++], dt,
                  theme::accent2, mem::is_attached())) {
    ui.addr_table.refresh_values();
    ui.toast("已刷新");
  }

  char path[128];
  std::snprintf(path, sizeof(path), "/data/local/tmp/memdbg_ct_%d.ct", getpid());
  if (anim_button("tsave", "保存地址表", ImVec2(-1, 0), ui.btns[bi++], dt,
                  theme::secondary, !ui.addr_table.entries.empty())) {
    int n = ui.addr_table.save(path);
    char buf[160];
    std::snprintf(buf, sizeof(buf), n >= 0 ? "已保存 %d 条 → %s" : "保存失败", n,
                  path);
    ui.toast(buf);
  }
  if (anim_button("tload", "加载地址表", ImVec2(-1, 0), ui.btns[bi++], dt,
                  theme::secondary)) {
    int n = ui.addr_table.load(path);
    char buf[160];
    std::snprintf(buf, sizeof(buf), n >= 0 ? "已加载 %d 条" : "加载失败", n);
    ui.toast(buf);
  }

  ImGui::Spacing();
  if (ImGui::BeginChild("ctable", ImVec2(0, 0), ImGuiChildFlags_Borders,
                        ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
    if (ui.addr_table.entries.empty())
      ImGui::TextDisabled("空表 · 从「结果」点加入表，或手动添加");
    for (int i = 0; i < (int)ui.addr_table.entries.size(); ++i) {
      auto& e = ui.addr_table.entries[i];
      ImGui::PushID(i);
      ImGui::Checkbox("##sel", &e.selected);
      ImGui::SameLine();
      ImGui::Checkbox("##frz", &e.freeze);
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (e.freeze) {
          uint64_t bits = 0;
          size_t sz = 0;
          if (mem::parse_value(e.type, e.value, bits, sz))
            e.freeze_bits = bits;
        }
      }
      ImGui::SameLine();
      ImGui::Text("0x%llX", (unsigned long long)e.addr);
      ImGui::SameLine();
      ImGui::TextColored(theme::accent, "%s", e.value);
      ImGui::SameLine();
      ImGui::TextDisabled("%s", e.desc);
      if (chip_button("浏览", theme::secondary)) {
        ui.browse_base = e.addr;
        std::snprintf(ui.edit_addr, sizeof(ui.edit_addr), "0x%llX",
                      (unsigned long long)e.addr);
        ui.tab = Tab::Addr; ui.addr_sub = 1;
        ui.tab_anim = 0.f;
        browse_reload(ui);
      }
      ImGui::SameLine();
      if (chip_button("指针扫", theme::peach)) {
        std::snprintf(ui.ptr_target, sizeof(ui.ptr_target), "0x%llX",
                      (unsigned long long)e.addr);
        ui.tab = Tab::Analyze; ui.analyze_sub = 0;
        ui.tab_anim = 0.f;
      }
      ImGui::PopID();
    }
  }
  ImGui::EndChild();
}

void tab_pointer(Ui& ui, float dt, int& bi) {
  ImGui::TextColored(theme::muted, "指针扫描 · 偏移模板 · 重解析");
  if (!mem::is_attached())
    ImGui::TextColored(theme::warn, "请先附加进程");

  draw_ime_field(ui, "ptgt", "目标地址", ui.ptr_target, sizeof(ui.ptr_target),
                 "指针目标", "hex", dt);
  ImGui::SliderInt("最大级数", &ui.ptr_level, 1, 5);
  ImGui::SliderInt("最大偏移", &ui.ptr_maxoff, 0x40, 0x4000, "0x%X");
  ImGui::Checkbox("仅静态基址(.so)", &ui.ptr_static);

  bool can = mem::is_attached() && !mem::ptrscan_busy();
  float half = (ImGui::GetContentRegionAvail().x - 8.f) * 0.5f;
  if (anim_button("pstart", "开始指针扫描", ImVec2(half, 0), ui.btns[bi++], dt,
                  theme::accent, can)) {
    mem::PtrScanConfig cfg;
    if (!mem::parse_addr(ui.ptr_target, cfg.target)) {
      ui.toast("目标地址无效");
    } else {
      cfg.max_level = ui.ptr_level;
      cfg.max_offset = (uint32_t)ui.ptr_maxoff;
      cfg.static_only = ui.ptr_static;
      cfg.max_results = 200;
      if (mem::ptrscan_start(cfg)) {
        ui.ptr_scanning = true;
        ui.ptr_selected = -1;
        ui.toast("指针扫描已开始…");
      } else
        ui.toast(mem::ptrscan_status());
    }
  }
  ImGui::SameLine();
  if (anim_button("pclear", "清空结果", ImVec2(half, 0), ui.btns[bi++], dt,
                  theme::danger)) {
    mem::ptrscan_clear();
    ui.ptr_results.clear();
    ui.ptr_selected = -1;
    ui.toast("已清空");
  }

  if (anim_button("pverify", "全部重解析/验证", ImVec2(half, 0), ui.btns[bi++],
                  dt, theme::teal, mem::is_attached() && !ui.ptr_results.empty())) {
    int ok = mem::ptrscan_verify_all(ui.ptr_results);
    char b[64];
    std::snprintf(b, sizeof(b), "有效 %d / %d", ok, (int)ui.ptr_results.size());
    ui.toast(b);
  }
  ImGui::SameLine();
  if (anim_button("prebind", "按模块重定位", ImVec2(half, 0), ui.btns[bi++], dt,
                  theme::peach, mem::is_attached() && !ui.ptr_results.empty())) {
    int n = 0;
    for (auto& c : ui.ptr_results)
      if (mem::ptrscan_rebind(c)) n++;
    char b[64];
    std::snprintf(b, sizeof(b), "重定位 %d 条", n);
    ui.toast(b);
  }

  draw_ime_field(ui, "psave", "存盘路径", ui.ptr_save_path,
                 sizeof(ui.ptr_save_path), "指针存盘", "text", dt);
  if (anim_button("psv", "保存指针模板", ImVec2(half, 0), ui.btns[bi++], dt,
                  theme::secondary, !ui.ptr_results.empty())) {
    int n = mem::ptrscan_save(ui.ptr_save_path, ui.ptr_results);
    char b[96];
    std::snprintf(b, sizeof(b), n >= 0 ? "已存 %d 条" : "保存失败", n);
    ui.toast(b);
  }
  ImGui::SameLine();
  if (anim_button("pld", "加载指针模板", ImVec2(half, 0), ui.btns[bi++], dt,
                  theme::secondary)) {
    int n = mem::ptrscan_load(ui.ptr_save_path, ui.ptr_results);
    char b[96];
    std::snprintf(b, sizeof(b), n >= 0 ? "加载 %d 条" : "加载失败", n);
    ui.toast(b);
  }

  draw_ime_field(ui, "ptpl", "模板 (lib.so+0xRVA,off0,off1)", ui.ptr_template_buf,
                 sizeof(ui.ptr_template_buf), "指针模板", "text", dt);
  if (anim_button("padd", "添加/解析模板", ImVec2(-1, 0), ui.btns[bi++], dt,
                  theme::accent2)) {
    mem::PtrChain c;
    if (mem::ptrscan_parse_template(ui.ptr_template_buf, c)) {
      mem::ptrscan_rebind(c);
      uintptr_t r = 0;
      c.valid = mem::ptrscan_resolve(c, r);
      if (c.valid) c.resolved = r;
      ui.ptr_results.push_back(c);
      ui.toast(c.valid ? "模板有效" : "模板已加(当前无效)");
    } else
      ui.toast("模板格式错误");
  }

  if (mem::ptrscan_busy() || ui.ptr_scanning) {
    ImGui::TextColored(theme::warn, "扫描中 %.0f%% · %s",
                       mem::ptrscan_progress() * 100.f, mem::ptrscan_status());
  } else {
    ImGui::TextDisabled("%s · %zu 条", mem::ptrscan_status(),
                       ui.ptr_results.size());
  }

  // 选中链的路径图
  if (ui.ptr_selected >= 0 && ui.ptr_selected < (int)ui.ptr_results.size()) {
    auto& c = ui.ptr_results[ui.ptr_selected];
    char fmt[256];
    mem::ptrscan_format(c, fmt, sizeof(fmt));
    ImGui::TextColored(theme::accent, "选中: %s", fmt);
    std::vector<uintptr_t> path;
    if (mem::ptrscan_resolve_path(c, path)) {
      ImGui::TextDisabled("指针路径:");
      for (size_t i = 0; i < path.size(); ++i) {
        ImGui::Text("  [%zu] 0x%llX %s", i, (unsigned long long)path[i],
                    i == 0 ? "(槽)" : (i + 1 == path.size() ? "(终点)" : ""));
        if (i + 1 < path.size()) {
          ImGui::SameLine();
          ImGui::TextColored(theme::muted, "──+0x%X──▶",
                             (unsigned)c.offsets[i]);
        }
      }
      if (chip_button("打开结构@终点", theme::peach)) {
        ui.structure.base = path.back();
        std::snprintf(ui.struct_base_buf, sizeof(ui.struct_base_buf), "0x%llX",
                      (unsigned long long)path.back());
        ui.structure.auto_dissect(ui.struct_auto_n);
        ui.tab = Tab::Analyze; ui.analyze_sub = 1;
        ui.tab_anim = 0.f;
      }
      ImGui::SameLine();
      if (chip_button("复制模板到输入框", theme::bubble)) {
        // 可解析格式
        if (!c.module.empty() && c.module != "anon") {
          int n = std::snprintf(ui.ptr_template_buf, sizeof(ui.ptr_template_buf),
                                "%s+0x%llX", c.module.c_str(),
                                (unsigned long long)c.base_rva);
          for (int32_t off : c.offsets)
            n += std::snprintf(ui.ptr_template_buf + n,
                               sizeof(ui.ptr_template_buf) - (size_t)n, ",0x%X",
                               (unsigned)off);
        }
      }
    } else {
      ImGui::TextColored(theme::danger, "当前无法解析此链");
    }
  }

  ImGui::Spacing();
  if (ImGui::BeginChild("ptrlist", ImVec2(0, 0), ImGuiChildFlags_Borders,
                        ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
    if (ui.ptr_results.empty())
      ImGui::TextDisabled("无指针链 · 扫描 / 加载模板 / 手动添加");
    for (int i = 0; i < (int)ui.ptr_results.size(); ++i) {
      auto& c = ui.ptr_results[i];
      ImGui::PushID(i);
      char line[256];
      mem::ptrscan_format(c, line, sizeof(line));
      bool sel = ui.ptr_selected == i;
      if (ImGui::Selectable(line, sel)) ui.ptr_selected = i;
      uintptr_t resolved = 0;
      bool ok = mem::ptrscan_resolve(c, resolved);
      c.valid = ok;
      if (ok) c.resolved = resolved;
      ImGui::SameLine();
      if (ok)
        ImGui::TextColored(theme::success, "[=0x%llX]",
                           (unsigned long long)resolved);
      else
        ImGui::TextColored(theme::danger, "[失效]");
      if (chip_button("加入表", theme::accent2)) {
        uintptr_t r = 0;
        if (mem::ptrscan_resolve(c, r))
          ui.addr_table.add(r, to_mem_type(ui.vtype), "指针链");
        else
          ui.addr_table.add(c.base, to_mem_type(ui.vtype), "指针基址");
        ui.toast("已加入地址表");
      }
      ImGui::SameLine();
      if (chip_button("浏览终点", theme::secondary)) {
        uintptr_t r = 0;
        if (mem::ptrscan_resolve(c, r)) {
          ui.browse_base = r;
          std::snprintf(ui.edit_addr, sizeof(ui.edit_addr), "0x%llX",
                        (unsigned long long)r);
          ui.tab = Tab::Addr; ui.addr_sub = 1;
          browse_reload(ui);
        }
      }
      ImGui::SameLine();
      if (chip_button("结构", theme::peach)) {
        uintptr_t r = 0;
        if (mem::ptrscan_resolve(c, r)) {
          ui.structure.base = r;
          std::snprintf(ui.struct_base_buf, sizeof(ui.struct_base_buf),
                        "0x%llX", (unsigned long long)r);
          ui.structure.auto_dissect(ui.struct_auto_n);
          ui.tab = Tab::Analyze; ui.analyze_sub = 1;
          ui.tab_anim = 0.f;
        }
      }
      ImGui::Separator();
      ImGui::PopID();
    }
  }
  ImGui::EndChild();
}

void tab_struct(Ui& ui, float dt, int& bi) {
  ImGui::TextColored(theme::muted, "结构体解析 · Structure Dissect");
  if (!mem::is_attached())
    ImGui::TextColored(theme::warn, "请先附加进程");

  draw_ime_field(ui, "sbase", "结构基址", ui.struct_base_buf,
                 sizeof(ui.struct_base_buf), "结构基址", "hex", dt);
  draw_ime_field(ui, "sname", "结构名", ui.structure.name,
                 sizeof(ui.structure.name), "结构名", "text", dt);

  float half = (ImGui::GetContentRegionAvail().x - 8.f) * 0.5f;
  if (anim_button("sopen", "打开/设置基址", ImVec2(half, 0), ui.btns[bi++], dt,
                  theme::accent, mem::is_attached())) {
    uintptr_t a = 0;
    if (mem::parse_addr(ui.struct_base_buf, a)) {
      ui.structure.base = a;
      if (ui.structure.fields.empty())
        ui.structure.auto_dissect(ui.struct_auto_n);
      else
        ui.structure.refresh();
      ui.toast("结构已打开");
    } else
      ui.toast("基址无效");
  }
  ImGui::SameLine();
  if (anim_button("sauto", "自动剖析", ImVec2(half, 0), ui.btns[bi++], dt,
                  theme::peach, mem::is_attached())) {
    uintptr_t a = 0;
    if (mem::parse_addr(ui.struct_base_buf, a)) ui.structure.base = a;
    ui.structure.auto_dissect(ui.struct_auto_n);
    ui.toast("已自动生成字段");
  }
  ImGui::SliderInt("自动字段数", &ui.struct_auto_n, 4, 48);
  if (anim_button("sref", "刷新字段值", ImVec2(-1, 0), ui.btns[bi++], dt,
                  theme::teal, mem::is_attached() && ui.structure.base)) {
    ui.structure.refresh();
    ui.toast("已刷新");
  }

  ImGui::Spacing();
  ImGui::TextColored(theme::muted, "添加字段");
  draw_ime_field(ui, "sfname", "字段名", ui.struct_field_name,
                 sizeof(ui.struct_field_name), "字段名", "text", dt);
  if (ImGui::BeginCombo("##sftype",
                        mem::field_type_name((mem::FieldType)ui.struct_field_type))) {
    for (int i = 0; i < (int)mem::FieldType::COUNT; ++i) {
      bool sel = ui.struct_field_type == i;
      if (ImGui::Selectable(mem::field_type_name((mem::FieldType)i), sel))
        ui.struct_field_type = i;
      if (sel) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  if (anim_button("sadd", "追加字段", ImVec2(half, 0), ui.btns[bi++], dt,
                  theme::accent2)) {
    ui.structure.append_field(ui.struct_field_name,
                              (mem::FieldType)ui.struct_field_type);
    ui.structure.refresh();
  }
  ImGui::SameLine();
  if (anim_button("sdel", "删除选中字段", ImVec2(half, 0), ui.btns[bi++], dt,
                  theme::danger)) {
    ui.structure.remove_selected();
    ui.toast("已删除");
  }

  draw_ime_field(ui, "ssave", "结构存盘路径", ui.struct_save_path,
                 sizeof(ui.struct_save_path), "结构存盘", "text", dt);
  if (anim_button("ssv", "保存结构", ImVec2(half, 0), ui.btns[bi++], dt,
                  theme::secondary)) {
    int n = ui.structure.save(ui.struct_save_path);
    char b[64];
    std::snprintf(b, sizeof(b), n >= 0 ? "已存 %d 字段" : "保存失败", n);
    ui.toast(b);
  }
  ImGui::SameLine();
  if (anim_button("sld", "加载结构", ImVec2(half, 0), ui.btns[bi++], dt,
                  theme::secondary)) {
    int n = ui.structure.load(ui.struct_save_path);
    if (n >= 0) {
      std::snprintf(ui.struct_base_buf, sizeof(ui.struct_base_buf), "0x%llX",
                    (unsigned long long)ui.structure.base);
      char b[64];
      std::snprintf(b, sizeof(b), "加载 %d 字段", n);
      ui.toast(b);
    } else
      ui.toast("加载失败");
  }

  ImGui::TextDisabled("基址 0x%llX · 约 %d 字节 · %d 字段",
                     (unsigned long long)ui.structure.base,
                     ui.structure.total_size(), (int)ui.structure.fields.size());

  ImGui::Spacing();
  if (ImGui::BeginChild("sfields", ImVec2(0, 0), ImGuiChildFlags_Borders,
                        ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
    if (ui.structure.fields.empty())
      ImGui::TextDisabled("无字段 · 打开基址并自动剖析，或手动追加");
    for (int i = 0; i < (int)ui.structure.fields.size(); ++i) {
      auto& f = ui.structure.fields[i];
      ImGui::PushID(i);
      ImGui::Checkbox("##sel", &f.selected);
      ImGui::SameLine();
      ImGui::Text("+0x%X  %-6s  %s", f.offset, mem::field_type_name(f.type),
                  f.name);
      ImGui::SameLine();
      ImGui::TextColored(theme::accent, "= %s", f.value);
      if (f.type == mem::FieldType::Ptr && f.as_ptr) {
        if (chip_button("跟随", theme::peach)) {
          ui.structure.base = f.as_ptr;
          std::snprintf(ui.struct_base_buf, sizeof(ui.struct_base_buf),
                        "0x%llX", (unsigned long long)f.as_ptr);
          ui.structure.auto_dissect(ui.struct_auto_n);
          ui.toast("已跟随指针");
        }
        ImGui::SameLine();
        if (chip_button("浏览", theme::secondary)) {
          ui.browse_base = f.as_ptr;
          std::snprintf(ui.edit_addr, sizeof(ui.edit_addr), "0x%llX",
                        (unsigned long long)f.as_ptr);
          ui.tab = Tab::Addr; ui.addr_sub = 1;
          browse_reload(ui);
        }
        ImGui::SameLine();
        if (chip_button("加入表", theme::accent2)) {
          ui.addr_table.add(f.as_ptr, mem::ValType::I64, f.name);
        }
      } else {
        if (chip_button("加入表", theme::accent2)) {
          ui.addr_table.add(ui.structure.base + (uintptr_t)f.offset,
                            to_mem_type(ui.vtype), f.name);
        }
        ImGui::SameLine();
        if (chip_button("浏览", theme::secondary)) {
          ui.browse_base = ui.structure.base + (uintptr_t)f.offset;
          std::snprintf(ui.edit_addr, sizeof(ui.edit_addr), "0x%llX",
                        (unsigned long long)ui.browse_base);
          ui.tab = Tab::Addr; ui.addr_sub = 1;
          browse_reload(ui);
        }
      }
      // 改类型快捷
      if (ImGui::BeginCombo("##ty", mem::field_type_name(f.type))) {
        for (int t = 0; t < (int)mem::FieldType::COUNT; ++t) {
          bool s = (int)f.type == t;
          if (ImGui::Selectable(mem::field_type_name((mem::FieldType)t), s)) {
            f.type = (mem::FieldType)t;
            ui.structure.refresh();
          }
        }
        ImGui::EndCombo();
      }
      ImGui::Separator();
      ImGui::PopID();
    }
  }
  ImGui::EndChild();
}

void do_attach_proc(Ui& ui, const mem::ProcInfo& p) {
  if (mem::is_attached() && mem::attached_pid() == p.pid) {
    ui.toast("已附加此进程");
    return;
  }
  if (mem::is_attached()) {
    mem::soft_bp_clear_all();
    mem::bp_shutdown();
    mem::detach();
  }
  if (mem::attach(p.pid)) {
    ui.attached_pid = p.pid;
    std::snprintf(ui.attached_name, sizeof(ui.attached_name), "%s",
                  p.name.c_str());
    char buf[160];
    std::snprintf(buf, sizeof(buf), "已附加 %d %s", p.pid, p.name.c_str());
    ui.toast(buf);
    // 预热符号（注入/变速/反汇编）
    mem::sym_refresh();
  } else {
    ui.attached_pid = -1;
    ui.attached_name[0] = 0;
    ui.toast("附加失败（需 root / 进程仍存活）");
  }
}

void tab_process(Ui& ui, float dt, int& bi) {
  ImGui::TextColored(theme::muted, "目标进程（需 root）");
  if (mem::is_attached()) {
    ImGui::TextColored(theme::success, "已附加: %d  %s", mem::attached_pid(),
                       mem::attached_name());
  } else {
    ImGui::TextColored(theme::warn, "未附加 · 刷新后点选进程，再「附加」或双击");
  }

  if (ImGui::Checkbox("过滤无图标进程", &ui.skip_no_icon))
    refresh_process_list(ui);
  ImGui::SameLine();
  if (ImGui::Checkbox("只保留腾讯进程", &ui.tencent_only))
    refresh_process_list(ui);
  ImGui::TextDisabled("模糊：包名/名子串 · 多关键词空格 · 双击行可直接附加");

  draw_ime_field(ui, "pfilter", "模糊搜索 包名/PID", ui.process_filter,
                 sizeof(ui.process_filter), "进程过滤", "text", dt);

  float half = (ImGui::GetContentRegionAvail().x - 8.f) * 0.5f;
  if (anim_button("refresh", "刷新列表", ImVec2(half, 0), ui.btns[bi++], dt,
                  theme::accent)) {
    refresh_process_list(ui);
    char b[48];
    std::snprintf(b, sizeof(b), "进程 %d 个", (int)ui.procs.size());
    ui.toast(b);
  }
  ImGui::SameLine();
  bool can_attach =
      ui.selected_list >= 0 && ui.selected_list < (int)ui.procs.size();
  if (anim_button("attach", "附加选中", ImVec2(half, 0), ui.btns[bi++], dt,
                  theme::success, can_attach)) {
    do_attach_proc(ui, ui.procs[ui.selected_list]);
  }
  if (anim_button("detach", "断开附加", ImVec2(-1, 0), ui.btns[bi++], dt,
                  theme::danger, mem::is_attached())) {
    mem::soft_bp_clear_all();
    mem::bp_shutdown();
    mem::speed_disable();
    mem::detach();
    ui.attached_pid = -1;
    ui.attached_name[0] = 0;
    ui.results.clear();
    ui.result_count = 0;
    ui.search_round = 0;
    ui.has_hit = false;
    ui.float_wins.clear();
    ui.toast("已断开");
  }

  ImGui::Spacing();
  if (ImGui::BeginChild("plist", ImVec2(0, 0), ImGuiChildFlags_Borders,
                        ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
    if (ui.procs.empty()) {
      ImGui::TextDisabled("无进程 · 点「刷新」或取消「过滤无图标」");
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float icon_sz = 40.f;
    for (int i = 0; i < (int)ui.procs.size(); ++i) {
      auto& pi = ui.procs[i];
      ImGui::PushID(i);
      bool sel = ui.selected_list == i;
      bool is_cur =
          mem::is_attached() && mem::attached_pid() == pi.pid;
      ImVec2 row0 = ImGui::GetCursorScreenPos();
      if (is_cur) {
        ImVec2 r1(row0.x + ImGui::GetContentRegionAvail().x,
                  row0.y + icon_sz + 4.f);
        dl->AddRectFilled(row0, r1, IM_COL32(40, 120, 80, 50), 6.f);
      }
      draw_proc_icon(dl, row0, icon_sz, pi);
      ImGui::Dummy(ImVec2(icon_sz + 8.f, icon_sz));
      ImGui::SameLine();
      ImGui::BeginGroup();
      char line[192];
      std::snprintf(line, sizeof(line), "%d  %s", pi.pid, pi.name.c_str());
      if (ImGui::Selectable(line, sel || is_cur, ImGuiSelectableFlags_AllowDoubleClick,
                            ImVec2(0, icon_sz - 4.f))) {
        ui.selected_list = i;
        if (ImGui::IsMouseDoubleClicked(0)) do_attach_proc(ui, pi);
      }
      if (pi.is_tencent) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.15f, 0.75f, 0.45f, 1.f), "腾讯");
      }
      if (is_cur) {
        ImGui::SameLine();
        ImGui::TextColored(theme::success, "已附加");
      }
      if (!pi.has_icon) {
        ImGui::SameLine();
        ImGui::TextDisabled("无图标");
      }
      ImGui::EndGroup();
      ImGui::PopID();
    }
  }
  ImGui::EndChild();
}

void tab_auto(Ui& ui, float dt, int& bi) {
  static const char* k[] = {"变速", "热键", "Lua", "AA", "Trainer"};
  draw_sub_nav(ui, ui.auto_sub, k, 5, dt, bi);
  if (!mem::is_attached())
    ImGui::TextColored(theme::warn, "Speedhack / AA / Lua 写内存需先附加");

  float half = (ImGui::GetContentRegionAvail().x - 8.f) * 0.5f;
  const int sub = ui.auto_sub;

  if (sub == 0) {
    ImGui::TextColored(theme::peach, "Speedhack");
    ImGui::TextWrapped("%s", mem::speed_status());
    ImGui::TextDisabled(
        "HARD=钩 clock_gettime(Δt×倍数) · SOFT=仅加速冻结写回");
    ImGui::TextDisabled("需先附加；HARD 失败会自动 SOFT，不会报死错");
    ImGui::SliderFloat("倍数", &ui.speed_mult, 0.25f, 10.f, "%.2fx");
    if (anim_button("spd_on", "应用变速", ImVec2(half, 0), ui.btns[bi++], dt,
                    theme::accent, mem::is_attached())) {
      // 先刷符号，提高 hard 成功率
      if (mem::sym_count() == 0) mem::sym_refresh();
      mem::speed_set(ui.speed_mult);
      ui.toast(mem::speed_status());
    }
    ImGui::SameLine();
    if (anim_button("spd_off", "关闭变速", ImVec2(half, 0), ui.btns[bi++], dt,
                    theme::danger, true)) {
      mem::speed_disable();
      ui.toast(mem::speed_status());
    }
    float presets[] = {0.5f, 1.f, 2.f, 3.f, 5.f, 10.f};
    for (int i = 0; i < 6; ++i) {
      if (i) ImGui::SameLine();
      char lab[16];
      std::snprintf(lab, sizeof(lab), "%.1fx", presets[i]);
      if (chip_button(lab, std::fabs(ui.speed_mult - presets[i]) < 0.01f
                               ? theme::accent
                               : theme::idle)) {
        ui.speed_mult = presets[i];
        if (mem::is_attached()) mem::speed_set(presets[i]);
      }
    }
  }

  if (sub == 1) {
    ImGui::TextColored(theme::peach, "全局热键");
    draw_ime_field(ui, "hk_key", "键 volup/voldown/power", ui.hotkey_key,
                   sizeof(ui.hotkey_key), "热键", "text", dt);
    draw_ime_field(ui, "hk_act", "动作 speed/script/nop_pc", ui.hotkey_action,
                   sizeof(ui.hotkey_action), "动作", "text", dt);
    draw_ime_field(ui, "hk_arg", "参数", ui.hotkey_arg, sizeof(ui.hotkey_arg),
                   "参数", "text", dt);
    if (anim_button("hk_init", "初始化设备", ImVec2(half, 0), ui.btns[bi++], dt,
                    theme::teal, true))
      ui.toast(mem::hotkey_init() ? "热键就绪" : "打开 input 失败");
    ImGui::SameLine();
    if (anim_button("hk_add", "添加绑定", ImVec2(half, 0), ui.btns[bi++], dt,
                    theme::accent, true)) {
      int kc = mem::hotkey_parse_key(ui.hotkey_key);
      if (kc < 0) ui.toast("未知键");
      else if (mem::hotkey_add(kc, ui.hotkey_action, ui.hotkey_arg))
        ui.toast("已绑定");
      else ui.toast("失败");
    }
    auto hks = mem::hotkey_list();
    for (int i = 0; i < (int)hks.size(); ++i) {
      ImGui::PushID(i + 9000);
      ImGui::Text("%s → %s %s", mem::hotkey_key_name(hks[i].keycode),
                  hks[i].action, hks[i].arg);
      ImGui::SameLine();
      if (chip_button("删", theme::danger)) mem::hotkey_remove(i);
      ImGui::PopID();
    }
  }

  if (sub == 2) {
    ImGui::TextColored(theme::peach, "Lua 5.4（全工具 API）");
    ImGui::TextColored(theme::warn,
                       "点示例 → 覆盖写入 /data/local/tmp/memdbg_example.lua");

    // 固定单一文件，每次示例覆盖写入
    static const char* kExPath = "/data/local/tmp/memdbg_example.lua";
    auto put_ex = [&](const char* /*name*/, const char* code) {
      std::snprintf(ui.script_buf, sizeof(ui.script_buf), "%s", code);
      FILE* f = std::fopen(kExPath, "w");
      if (f) {
        std::fputs(code, f);
        if (code[0] && code[std::strlen(code) - 1] != '\n') std::fputc('\n', f);
        std::fclose(f);
        std::snprintf(ui.script_path, sizeof(ui.script_path), "%s", kExPath);
        ui.toast("已写 /data/local/tmp/memdbg_example.lua");
      } else {
        ui.toast("写 tmp 失败（编辑框已更新）");
      }
    };

    // 分类条：始终显示（不再默认折叠）
    {
      struct Cat {
        const char* name;
        int id;
      };
      static const Cat cats[] = {
          {"读写", 1},   {"扫描", 2}, {"指针", 3},
          {"调试", 4},   {"补丁", 5}, {"变速", 6},
          {"反汇", 7},   {"综合", 8}, {"帮助", 9},
      };
      float gap = 4.f;
      float cw = (ImGui::GetContentRegionAvail().x - gap * 2.f) / 3.f;
      if (cw < 56.f) cw = 56.f;
      for (int i = 0; i < 9; ++i) {
        if (i % 3) ImGui::SameLine(0, gap);
        char id[32];
        std::snprintf(id, sizeof(id), "lcat%d", cats[i].id);
        bool on = (ui.lua_ex_cat == cats[i].id);
        ImVec4 c = on ? theme::accent : theme::tab_idle;
        if (anim_button(id, cats[i].name, ImVec2(cw, 0), ui.btns[bi++], dt, c,
                        true)) {
          ui.lua_ex_cat = cats[i].id;  // 点分类即切换子示例
        }
      }
    }

    // 保证至少有一个分类选中
    if (ui.lua_ex_cat < 1 || ui.lua_ex_cat > 9) ui.lua_ex_cat = 1;

    // 示例按钮限高滚动，把纵向空间留给下方编辑器/日志（随窗口上下缩放）
    {
      float ex_cap = ImGui::GetContentRegionAvail().y * 0.26f;
      if (ex_cap < 90.f) ex_cap = 90.f;
      if (ex_cap > 180.f) ex_cap = 180.f;
      ImGui::BeginChild("lua_examples", ImVec2(0, ex_cap), ImGuiChildFlags_Borders,
                        ImGuiWindowFlags_AlwaysVerticalScrollbar);
    }

    if (ui.lua_ex_cat == 1) {
      ImGui::TextColored(theme::muted, "读写 · 点按钮覆盖 memdbg_example.lua");
      if (anim_button("s1a", "写 i32 + 读回", ImVec2(half, 0), ui.btns[bi++],
                      dt, theme::idle, true))
        put_ex("s1a", "-- 改 a 为真实地址\n"
               "local a = 0x0\n"
               "mem.write_i32(a, 999)\n"
               "print(\"i32=\", mem.read_i32(a))\n");
      ImGui::SameLine();
      if (anim_button("s1b", "写 f32/f64", ImVec2(half, 0), ui.btns[bi++], dt,
                      theme::idle, true))
        put_ex("s1b", "local a = 0x0\n"
               "mem.write_f32(a, 99.5)\n"
               "mem.write_f64(a+8, 3.1415926)\n"
               "print(mem.read_f32(a), mem.read_f64(a+8))\n");
      if (anim_button("s1c", "写字符串/hex", ImVec2(half, 0), ui.btns[bi++],
                      dt, theme::idle, true))
        put_ex("s1c", "local a = 0x0\n"
               "mem.write_str(a, \"HelloQQ\")\n"
               "print(mem.read_str(a))\n"
               "mem.write_hex(a, \"48 69 00\")\n");
      ImGui::SameLine();
      if (anim_button("s1d", "读 bytes", ImVec2(half, 0), ui.btns[bi++], dt,
                      theme::idle, true))
        put_ex("s1d", "local a = 0x0\n"
               "local b = mem.read_bytes(a, 16)\n"
               "print(#b, b:byte(1,4))\n");
    }
    if (ui.lua_ex_cat == 2) {
      ImGui::TextColored(theme::muted, "扫描 / 冻结");
      if (anim_button("s2a", "首次精确扫描", ImVec2(half, 0), ui.btns[bi++],
                      dt, theme::idle, true))
        put_ex("s2a", "-- 值, 类型, 模式, 区域\n"
               "print(mem.scan_first(\"100\", \"i32\", \"exact\", \"anon\"))\n"
               "local ok, n, st = mem.scan_wait(60000)\n"
               "print(ok, n, st)\n"
               "local rs = mem.scan_results(20)\n"
               "for i,r in ipairs(rs) do print(i, r.addr, r.bits) end\n");
      ImGui::SameLine();
      if (anim_button("s2b", "再次未变筛选", ImVec2(half, 0), ui.btns[bi++],
                      dt, theme::idle, true))
        put_ex("s2b", "print(mem.scan_next(\"0\", \"i32\", \"unchanged\"))\n"
               "mem.scan_wait()\n"
               "print(\"count\", mem.scan_count(), mem.scan_status())\n");
      if (anim_button("s2c", "冻结/解冻", ImVec2(half, 0), ui.btns[bi++], dt,
                      theme::idle, true))
        put_ex("s2c", "local a = 0x0\n"
               "mem.freeze(a, \"i32\", 100)\n"
               "print(\"frozen\")\n"
               "-- mem.unfreeze(a)\n"
               "-- mem.clear_frozen()\n");
      ImGui::SameLine();
      if (anim_button("s2d", "UTF8 扫描", ImVec2(half, 0), ui.btns[bi++], dt,
                      theme::idle, true))
        put_ex("s2d", "mem.scan_first(\"hello\", \"utf8\", \"exact\", \"anon\")\n"
               "mem.scan_wait()\n"
               "print(mem.scan_count())\n");
      if (anim_button("s2e", "导出/Dump", ImVec2(-1, 0), ui.btns[bi++], dt,
                      theme::idle, true))
        put_ex("s2e", "print(mem.export_results(\"/data/local/tmp/rs.txt\"))\n"
               "print(mem.dump(0x0, 256, \"/data/local/tmp/d.bin\"))\n");
    }
    if (ui.lua_ex_cat == 3) {
      ImGui::TextColored(theme::muted, "指针 / 结构");
      if (anim_button("s3a", "指针扫描", ImVec2(half, 0), ui.btns[bi++], dt,
                      theme::idle, true))
        put_ex("s3a", "local target = 0x0  -- 目标地址\n"
               "local n, st = mem.ptrscan(target, 1, 0x800)\n"
               "print(n, st)\n"
               "for i,c in ipairs(mem.ptrscan_results(10)) do\n"
               "  print(i, c.text, c.module)\n"
               "end\n");
      ImGui::SameLine();
      if (anim_button("s3b", "结构剖析", ImVec2(half, 0), ui.btns[bi++], dt,
                      theme::idle, true))
        put_ex("s3b", "local base = 0x0\n"
               "local fs = mem.struct_dissect(base, 16)\n"
               "for i,f in ipairs(fs) do\n"
               "  print(f.offset, f.type, f.name, f.value)\n"
               "end\n");
      if (anim_button("s3c", "模块基址", ImVec2(-1, 0), ui.btns[bi++], dt,
                      theme::idle, true))
        put_ex("s3c", "local s,e = mem.module_base(\"libc.so\")\n"
               "print(s, e)\n"
               "for i,m in ipairs(mem.modules(\"lib\")) do\n"
               "  if i<=5 then print(m.start, m.path) end\n"
               "end\n");
    }
    if (ui.lua_ex_cat == 4) {
      ImGui::TextColored(theme::muted, "调试 / 断点");
      if (anim_button("s4a", "暂停+寄存器", ImVec2(half, 0), ui.btns[bi++],
                      dt, theme::idle, true))
        put_ex("s4a", "assert(mem.is_attached())\n"
               "mem.pause()\n"
               "local r = mem.regs()\n"
               "print(string.format(\"PC=%X SP=%X LR=%X\", r.pc, r.sp, r.lr))\n"
               "for i=0,7 do print(\"x\"..i, r[\"x\"..i]) end\n"
               "mem.resume()\n");
      ImGui::SameLine();
      if (anim_button("s4b", "硬件写观察点", ImVec2(half, 0), ui.btns[bi++],
                      dt, theme::idle, true))
        put_ex("s4b", "local a = 0x0\n"
               "mem.bp_init()\n"
               "local id, st = mem.bp_set(a, \"write\", 4)\n"
               "print(id, st)\n"
               "-- mem.bp_clear(id)\n");
      if (anim_button("s4c", "软断点+条件", ImVec2(half, 0), ui.btns[bi++],
                      dt, theme::idle, true))
        put_ex("s4c", "local a = 0x0  -- 代码地址\n"
               "local id = mem.soft_bp(a, \"x0==1\", false)\n"
               "print(\"soft\", id, mem.status())\n"
               "-- mem.soft_bp_clear(id)\n");
      ImGui::SameLine();
      if (anim_button("s4d", "单步/步过", ImVec2(half, 0), ui.btns[bi++], dt,
                      theme::idle, true))
        put_ex("s4d", "mem.pause()\n"
               "mem.step()\n"
               "print(\"pc\", mem.regs().pc)\n"
               "mem.step_over()\n"
               "mem.resume()\n");
      if (anim_button("s4e", "栈/Trace", ImVec2(-1, 0), ui.btns[bi++], dt,
                      theme::idle, true))
        put_ex("s4e", "mem.pause()\n"
               "for i,a in ipairs(mem.stack(12)) do print(i, a) end\n"
               "for i,t in ipairs(mem.trace(8)) do print(t.pc, t.text) end\n"
               "mem.resume()\n");
    }
    if (ui.lua_ex_cat == 5) {
      ImGui::TextColored(theme::muted, "补丁 / 注入");
      if (anim_button("s5a", "NOP / 汇编", ImVec2(half, 0), ui.btns[bi++], dt,
                      theme::idle, true))
        put_ex("s5a", "local a = 0x0  -- 谨慎：可能崩目标\n"
               "-- mem.nop(a, 2)\n"
               "local w, hex = mem.assemble(\"mov x0, #0\")\n"
               "print(w, hex)\n"
               "-- mem.patch_asm(a, \"ret\")\n");
      ImGui::SameLine();
      if (anim_button("s5b", "find_dlopen", ImVec2(half, 0), ui.btns[bi++],
                      dt, theme::idle, true))
        put_ex("s5b", "local a, st = mem.find_dlopen()\n"
               "print(string.format(\"0x%X\", a), st)\n");
      if (anim_button("s5c", "remote_call getpid", ImVec2(half, 0),
                      ui.btns[bi++], dt, theme::idle, true))
        put_ex("s5c", "local fn, mod = mem.sym_find(\"getpid\")\n"
               "print(fn, mod)\n"
               "if fn then print(\"ret\", mem.remote_call(fn, {})) end\n");
      ImGui::SameLine();
      if (anim_button("s5d", "inject_so", ImVec2(half, 0), ui.btns[bi++], dt,
                      theme::idle, true))
        put_ex("s5d", "-- 路径须目标可读\n"
               "-- local h = mem.inject_so(\"/data/local/tmp/libx.so\")\n"
               "-- print(h)\n"
               "print(\"see mem.find_dlopen / inject_so\")\n");
    }
    if (ui.lua_ex_cat == 6) {
      ImGui::TextColored(theme::muted, "变速 / AA");
      if (anim_button("s6a", "变速 2x", ImVec2(half, 0), ui.btns[bi++], dt,
                      theme::idle, true))
        put_ex("s6a", "print(mem.speed(2.0))\n"
               "mem.sleep(1000)\n"
               "print(mem.speed(\"off\"))\n");
      ImGui::SameLine();
      if (anim_button("s6b", "AA 示例", ImVec2(half, 0), ui.btns[bi++], dt,
                      theme::idle, true))
        put_ex("s6b", "local aa = [[\n"
               "[ENABLE]\n"
               "alloc(buf, 64)\n"
               "registersymbol(buf)\n"
               "[DISABLE]\n"
               "dealloc(buf)\n"
               "]]\n"
               "print(mem.aa(aa, true))\n"
               "mem.aa(aa, false)\n");
    }
    if (ui.lua_ex_cat == 7) {
      ImGui::TextColored(theme::muted, "反汇编 / 符号");
      if (anim_button("s7a", "disasm", ImVec2(half, 0), ui.btns[bi++], dt,
                      theme::idle, true))
        put_ex("s7a", "local s, e = mem.module_base(\"libc.so\")\n"
               "if not s then print(\"no libc\"); return end\n"
               "for i,ins in ipairs(mem.disasm(s + 0x1000, 12)) do\n"
               "  print(string.format(\"0x%X\", ins.addr), ins.mnem, ins.ops)\n"
               "end\n");
      ImGui::SameLine();
      if (anim_button("s7b", "伪 C", ImVec2(half, 0), ui.btns[bi++], dt,
                      theme::idle, true))
        put_ex("s7b", "local a = 0x0\n"
               "print(mem.pseudo_c(a, 48))\n");
      if (anim_button("s7c", "符号查找", ImVec2(-1, 0), ui.btns[bi++], dt,
                      theme::idle, true))
        put_ex("s7c", "print(\"syms\", mem.sym_refresh())\n"
               "local a, mod = mem.sym_find(\"open\")\n"
               "print(a, mod)\n"
               "print(mem.sym_name(a or 0))\n");
    }
    if (ui.lua_ex_cat == 8) {
      ImGui::TextColored(theme::muted, "综合");
      if (anim_button("s8a", "maps 过滤", ImVec2(half, 0), ui.btns[bi++], dt,
                      theme::idle, true))
        put_ex("s8a", "for i,r in ipairs(mem.maps(false, \"libc\")) do\n"
               "  if i<=8 then print(r.start, r.perms, r.path) end\n"
               "end\n");
      ImGui::SameLine();
      if (anim_button("s8b", "线程列表", ImVec2(half, 0), ui.btns[bi++], dt,
                      theme::idle, true))
        put_ex("s8b", "for i,t in ipairs(mem.threads()) do\n"
               "  if i<=15 then print(t.tid, t.state, t.name) end\n"
               "end\n");
      if (anim_button("s8c", "进程列表", ImVec2(half, 0), ui.btns[bi++], dt,
                      theme::idle, true))
        put_ex("s8c", "for i,p in ipairs(mem.list_procs(\"qq\")) do\n"
               "  print(p.pid, p.name, p.tencent)\n"
               "end\n");
      ImGui::SameLine();
      if (anim_button("s8d", "循环写", ImVec2(half, 0), ui.btns[bi++], dt,
                      theme::idle, true))
        put_ex("s8d", "local a = 0x0\n"
               "for i=1,5 do\n"
               "  mem.write_i32(a, i*10)\n"
               "  print(i, mem.read_i32(a))\n"
               "  mem.sleep(200)\n"
               "end\n");
    }
    if (ui.lua_ex_cat == 9) {
      ImGui::TextColored(theme::muted, "帮助");
      if (anim_button("s9a", "mem.help()", ImVec2(half, 0), ui.btns[bi++], dt,
                      theme::secondary, true))
        put_ex("s9a", "mem.help()\n");
      ImGui::SameLine();
      if (anim_button("s9b", "状态探测", ImVec2(half, 0), ui.btns[bi++], dt,
                      theme::idle, true))
        put_ex("s9b", "print(\"attached\", mem.is_attached(), mem.pid(), mem.name())\n"
               "print(\"scan\", mem.scan_count(), mem.scan_status())\n"
               "print(\"bp\", mem.status())\n"
               "print(\"speed\", mem.speed(\"off\"))\n");
    }
    ImGui::EndChild();  // lua_examples

    ImGui::Spacing();
    draw_ime_field(ui, "scpath", "脚本路径 .lua", ui.script_path,
                   sizeof(ui.script_path), "路径", "text", dt);

    // 编辑器 + 日志：严格吃满当前可见剩余高度（不强制超大 min，避免外层滚动“吃掉”上下缩放）
    {
      float rem = ImGui::GetContentRegionAvail().y;
      // 标题×2 + 运行行 + 清空行
      float reserve = 108.f;
      float split = rem - reserve;
      if (split < 80.f) split = std::max(60.f, rem * 0.55f);
      float ed_h = split * 0.50f;
      if (ed_h < 60.f) ed_h = 60.f;
      if (ed_h > split - 40.f) ed_h = std::max(40.f, split - 40.f);

      ImGui::TextColored(theme::muted, "脚本编辑（随窗口上下缩放）");
      ImGui::InputTextMultiline("##script", ui.script_buf, sizeof(ui.script_buf),
                                ImVec2(-1, ed_h),
                                ImGuiInputTextFlags_AllowTabInput);
      if (anim_button("sc_run", "运行 Lua", ImVec2(half, 0), ui.btns[bi++], dt,
                      theme::accent, true)) {
        char e[256];
        ui.toast(mem::script_run(ui.script_buf, e, sizeof(e))
                     ? "完成"
                     : (e[0] ? e : "失败"));
      }
      ImGui::SameLine();
      if (anim_button("sc_file", "运行文件", ImVec2(half, 0), ui.btns[bi++], dt,
                      theme::teal, true)) {
        char e[256];
        ui.toast(mem::script_run_file(ui.script_path, e, sizeof(e))
                     ? "完成"
                     : (e[0] ? e : "失败"));
      }
      if (anim_button("sc_clr", "清空日志", ImVec2(-1, 0), ui.btns[bi++], dt,
                      theme::secondary, true))
        mem::script_log_clear();

      ImGui::TextColored(theme::muted, "运行日志（随窗口上下缩放）");
      // 吃光剩余可见高度：拉高/压矮面板时日志区同步变化
      float log_fill = ImGui::GetContentRegionAvail().y - 2.f;
      if (log_fill < 48.f) log_fill = 48.f;
      if (ImGui::BeginChild("slog", ImVec2(0, log_fill), ImGuiChildFlags_Borders,
                            ImGuiWindowFlags_AlwaysVerticalScrollbar |
                                ImGuiWindowFlags_HorizontalScrollbar)) {
        if (mem::script_log()[0])
          ImGui::TextUnformatted(mem::script_log());
        else
          ImGui::TextDisabled("（运行后输出显示在这里 · 拖面板底边可拉高本框）");
      }
      ImGui::EndChild();
    }
  }

  if (sub == 3) {
    ImGui::TextColored(theme::peach, "Auto Assemble");
    ImGui::TextDisabled("%s · sym=%d", mem::aa_status(), mem::aa_symbol_count());
    {
      float aa_h = ImGui::GetContentRegionAvail().y - 100.f;
      if (aa_h < 120.f) aa_h = 120.f;
      ImGui::InputTextMultiline("##aa", ui.aa_buf, sizeof(ui.aa_buf),
                                ImVec2(-1, aa_h));
    }
    if (anim_button("aa_en", "启用 ENABLE", ImVec2(half, 0), ui.btns[bi++], dt,
                    theme::accent, mem::is_attached())) {
      char e[128];
      ui.toast(mem::aa_run(ui.aa_buf, true, e, sizeof(e)) ? mem::aa_status()
                                                          : (e[0] ? e : "失败"));
    }
    ImGui::SameLine();
    if (anim_button("aa_dis", "禁用/还原", ImVec2(half, 0), ui.btns[bi++], dt,
                    theme::danger, true)) {
      char e[128];
      mem::aa_run(ui.aa_buf, false, e, sizeof(e));
      ui.toast(mem::aa_status());
    }
  }

  if (sub == 4) {
    ImGui::TextColored(theme::peach, "Trainer 导出/导入");
    draw_ime_field(ui, "tr_name", "名称", ui.trainer_name, sizeof(ui.trainer_name),
                   "名", "text", dt);
    draw_ime_field(ui, "tr_pkg", "包名", ui.trainer_pkg, sizeof(ui.trainer_pkg),
                   "包名", "text", dt);
    draw_ime_field(ui, "tr_path", "路径", ui.trainer_path, sizeof(ui.trainer_path),
                   "路径", "text", dt);
    if (anim_button("tr_exp", "导出 .trainer", ImVec2(half, 0), ui.btns[bi++],
                    dt, theme::accent, !ui.addr_table.entries.empty())) {
      mem::TrainerMeta m;
      std::snprintf(m.name, sizeof(m.name), "%s", ui.trainer_name);
      std::snprintf(m.package, sizeof(m.package), "%s", ui.trainer_pkg);
      int n = mem::trainer_export(ui.addr_table, ui.trainer_path, m,
                                  ui.script_buf[0] ? ui.script_buf : nullptr);
      char b[160];
      std::snprintf(b, sizeof(b), n >= 0 ? "导出 %d" : "失败", n);
      ui.toast(b);
    }
    ImGui::SameLine();
    if (anim_button("tr_imp", "导入到地址表", ImVec2(half, 0), ui.btns[bi++],
                    dt, theme::teal, true)) {
      mem::TrainerMeta m;
      int n = mem::trainer_import(ui.addr_table, ui.trainer_path, &m);
      char b[160];
      std::snprintf(b, sizeof(b), n >= 0 ? "导入 %d · %s" : "失败", n, m.name);
      ui.toast(b);
    }
  }
}


void tab_tools(Ui& ui, float dt, int& bi) {
  // 由 tab_debug 设置 ui.debug_sub；此处只渲染对应区块
  if (!mem::is_attached())
    ImGui::TextColored(theme::warn, "部分功能需要先附加进程");
  else
    ImGui::TextColored(theme::success, "已附加 %d · %s", mem::attached_pid(),
                       mem::attached_name());

  float half = (ImGui::GetContentRegionAvail().x - 8.f) * 0.5f;
  const int sub = ui.debug_sub;

  // ── 0 控制：暂停/单步/寄存器/导出 ──
  if (sub == 0) {
    ImGui::TextColored(theme::muted, "执行控制");
    ImGui::TextWrapped("%s · tid=%d · %s", mem::bp_status(), mem::dbg_get_tid(),
                       mem::dbg_is_paused() ? "已暂停" : "运行中");
    if (anim_button("pause", "暂停", ImVec2(half, 0), ui.btns[bi++], dt,
                    theme::warn, mem::is_attached()))
      ui.toast(mem::dbg_pause() ? "已暂停" : mem::bp_status());
    ImGui::SameLine();
    if (anim_button("resume", "继续", ImVec2(half, 0), ui.btns[bi++], dt,
                    theme::success, mem::is_attached()))
      ui.toast(mem::dbg_resume() ? "已继续" : mem::bp_status());
    if (anim_button("step", "步入", ImVec2(half * 0.65f, 0), ui.btns[bi++], dt,
                    theme::peach, mem::is_attached())) {
      if (mem::dbg_step()) {
        mem::dbg_regs_read(ui.dbg_regs);
        ui.toast("步入");
      } else ui.toast(mem::bp_status());
    }
    ImGui::SameLine();
    if (anim_button("stepo", "步过", ImVec2(half * 0.65f, 0), ui.btns[bi++], dt,
                    theme::peach, mem::is_attached()))
      ui.toast(mem::dbg_step_over() ? "步过" : mem::bp_status());
    ImGui::SameLine();
    if (anim_button("stepu", "步出", ImVec2(half * 0.65f, 0), ui.btns[bi++], dt,
                    theme::peach, mem::is_attached()))
      ui.toast(mem::dbg_step_out() ? "步出" : mem::bp_status());
    if (anim_button("regs", "寄存器窗", ImVec2(half, 0), ui.btns[bi++], dt,
                    theme::accent, mem::is_attached())) {
      mem::dbg_regs_read(ui.dbg_regs);
      mem::dbg_fp_regs_read(ui.dbg_fp);
      spawn_float(ui, Ui::FloatKind::Registers, 0);
    }
    ImGui::SameLine();
    if (anim_button("trace", "Trace×32", ImVec2(half, 0), ui.btns[bi++], dt,
                    theme::warn, mem::is_attached())) {
      if (mem::dbg_trace(32, ui.trace_log)) {
        char b[48];
        std::snprintf(b, sizeof(b), "Trace %d", (int)ui.trace_log.size());
        ui.toast(b);
      } else ui.toast(mem::bp_status());
    }
    if (!ui.trace_log.empty() &&
        ImGui::BeginChild("trlog",
                          ImVec2(0, std::max(90.f,
                                             ImGui::GetContentRegionAvail().y *
                                                 0.35f)),
                          ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
      for (auto& e : ui.trace_log)
        ImGui::Text("%llX  %s", (unsigned long long)e.pc, e.text);
      ImGui::EndChild();
    }
    if (anim_button("stack", "调用栈", ImVec2(half, 0), ui.btns[bi++], dt,
                    theme::accent2, mem::is_attached())) {
      ui.stack_frames = mem::dbg_stack_trace(24);
      spawn_float(ui, Ui::FloatKind::StackTrace, 0);
    }
    ImGui::SameLine();
    if (anim_button("mods", "模块列表", ImVec2(half, 0), ui.btns[bi++], dt,
                    theme::secondary, mem::is_attached())) {
      ui.modules = mem::list_modules(
          ui.module_filter[0] ? ui.module_filter : nullptr);
      spawn_float(ui, Ui::FloatKind::Modules, 0);
    }
    draw_ime_field(ui, "mofilt", "模块过滤", ui.module_filter,
                   sizeof(ui.module_filter), "模块过滤", "text", dt);
    draw_ime_field(ui, "dsize", "Dump 字节数", ui.dump_size_buf,
                   sizeof(ui.dump_size_buf), "Dump 大小", "number", dt);
    if (anim_button("dump", "Dump 当前地址", ImVec2(-1, 0), ui.btns[bi++], dt,
                    theme::accent, mem::is_attached())) {
      uintptr_t addr = 0;
      if (!mem::parse_addr(ui.edit_addr, addr)) ui.toast("地址无效");
      else {
        size_t len = (size_t)std::strtoul(ui.dump_size_buf, nullptr, 0);
        if (len == 0) len = 4096;
        if (len > 8 * 1024 * 1024) len = 8 * 1024 * 1024;
        char path[128];
        std::snprintf(path, sizeof(path),
                      "/data/local/tmp/memdbg_dump_%d_%llx.bin", getpid(),
                      (unsigned long long)addr);
        int n = mem::dump_mem(addr, len, path);
        char buf[160];
        if (n > 0) std::snprintf(buf, sizeof(buf), "Dump %d → %s", n, path);
        else std::snprintf(buf, sizeof(buf), "Dump 失败");
        ui.toast(buf);
      }
    }
    if (anim_button("export", "导出扫描结果", ImVec2(-1, 0), ui.btns[bi++], dt,
                    theme::secondary, ui.result_count > 0)) {
      char path[128];
      std::snprintf(path, sizeof(path), "/data/local/tmp/memdbg_addrs_%d.txt",
                    getpid());
      int n = mem::export_results(path);
      char buf[160];
      std::snprintf(buf, sizeof(buf), n >= 0 ? "导出 %d → %s" : "导出失败", n,
                    path);
      ui.toast(buf);
    }
    if (anim_button("clear_fz", "清除全部冻结", ImVec2(half, 0), ui.btns[bi++],
                    dt, theme::warn)) {
      mem::clear_all_frozen();
      for (auto& r : ui.results) r.frozen = false;
      for (auto& e : ui.addr_table.entries) e.freeze = false;
      ui.toast("冻结已清除");
    }
    ImGui::SameLine();
    if (anim_button("exit", "退出程序", ImVec2(half, 0), ui.btns[bi++], dt,
                    theme::danger)) {
      request_exit(ui);
    }
    ImGui::TextDisabled("退出：点本按钮（已取消长按悬浮球退出）");
  }

  // ── 1 断点 ──
  if (sub == 1) {
    ImGui::TextColored(theme::muted, "硬件 / 软断点");
    ImGui::TextWrapped("%s · 后端 %s", mem::bp_status(), mem::bp_backend_name());
    ImGui::TextDisabled("Android 优先 ptrace 硬件槽；perf 常 ENOSPC 会自动回退");
    if (anim_button("bpinit", "初始化断点后端", ImVec2(-1, 0), ui.btns[bi++],
                    dt, theme::success, mem::is_attached())) {
      if (mem::bp_init()) {
        mem::bp_arm_and_continue();
        ui.toast(mem::bp_status());
      } else ui.toast(mem::bp_status());
    }
    // 快捷：当前地址硬件断点
    if (anim_button("hw_exec", "执行断点@地址", ImVec2(half, 0), ui.btns[bi++],
                    dt, theme::warn, mem::is_attached())) {
      uintptr_t a = 0;
      if (mem::parse_addr(ui.edit_addr, a)) {
        int id = mem::bp_set(a & ~3ull, mem::BpType::Exec, 4);
        ui.toast(id < 0 ? mem::bp_status() : mem::bp_status());
      } else ui.toast("地址无效");
    }
    ImGui::SameLine();
    if (anim_button("hw_w", "写观察点@地址", ImVec2(half, 0), ui.btns[bi++],
                    dt, theme::danger, mem::is_attached())) {
      uintptr_t a = 0;
      if (mem::parse_addr(ui.edit_addr, a)) {
        int id = mem::bp_set(a, mem::BpType::WatchW, 4);
        ui.toast(id < 0 ? mem::bp_status() : mem::bp_status());
      } else ui.toast("地址无效");
    }
    if (anim_button("bpwin", "断点管理窗口", ImVec2(half, 0), ui.btns[bi++], dt,
                    theme::accent))
      spawn_float(ui, Ui::FloatKind::Breakpoints, 0);
    ImGui::SameLine();
    if (anim_button("hitwin", "命中日志窗口", ImVec2(half, 0), ui.btns[bi++],
                    dt, theme::accent2))
      spawn_float(ui, Ui::FloatKind::HitLog, 0);
    if (anim_button("bpclear", "清除硬件断点", ImVec2(half, 0), ui.btns[bi++],
                    dt, theme::danger)) {
      mem::bp_clear_all();
      ui.toast("硬件断点已清");
    }
    ImGui::SameLine();
    if (anim_button("sbpc", "清除软断点", ImVec2(half, 0), ui.btns[bi++], dt,
                    theme::danger)) {
      mem::soft_bp_clear_all();
      ui.toast("软断点已清");
    }
    draw_ime_field(ui, "bpc", "条件 如 x0==1", ui.bp_cond_buf,
                   sizeof(ui.bp_cond_buf), "条件", "text", dt);
    if (anim_button("sbp", "软断点 @ 当前地址", ImVec2(half, 0), ui.btns[bi++],
                    dt, theme::accent, mem::is_attached())) {
      uintptr_t a = 0;
      if (mem::parse_addr(ui.edit_addr, a)) {
        int id = mem::soft_bp_set(a);
        char b[64];
        if (id < 0) ui.toast(mem::bp_status());
        else {
          std::snprintf(b, sizeof(b), "软断点#%d", id);
          ui.toast(b);
        }
      }
    }
    ImGui::SameLine();
    if (anim_button("sbpc2", "条件软断点", ImVec2(half, 0), ui.btns[bi++], dt,
                    theme::peach, mem::is_attached())) {
      uintptr_t a = 0;
      if (mem::parse_addr(ui.edit_addr, a)) {
        int id = mem::soft_bp_set_cond(a, ui.bp_cond_buf, false);
        ui.toast(id < 0 ? mem::bp_status() : "条件软断点已下");
      }
    }
    auto sbp = mem::soft_bp_list();
    if (!sbp.empty()) {
      ImGui::TextDisabled("软断点 %d", (int)sbp.size());
      for (auto& s : sbp) {
        ImGui::Text("#%d 0x%llX", s.id, (unsigned long long)s.addr);
        ImGui::SameLine();
        ImGui::PushID(s.id);
        if (chip_button("删", theme::danger)) mem::soft_bp_clear(s.id);
        ImGui::PopID();
      }
    }
    ImGui::TextColored(theme::muted, "线程 tid=%d", mem::dbg_get_tid());
    if (anim_button("thref", "刷新线程", ImVec2(half, 0), ui.btns[bi++], dt,
                    theme::teal, mem::is_attached()))
      ui.threads = mem::list_threads();
    ImGui::SameLine();
    if (anim_button("thmain", "主线程", ImVec2(half, 0), ui.btns[bi++], dt,
                    theme::secondary, mem::is_attached())) {
      mem::dbg_set_tid(0);
      ui.toast("主线程");
    }
    if (ImGui::BeginChild("thpick", ImVec2(0, 100), ImGuiChildFlags_Borders)) {
      for (auto& th : ui.threads) {
        ImGui::PushID(th.tid);
        ImGui::Text("%d [%s] %s", th.tid, th.state, th.name);
        ImGui::SameLine();
        if (chip_button("切换", theme::bubble)) {
          mem::dbg_attach_thread(th.tid);
          mem::dbg_regs_read(ui.dbg_regs);
          ui.toast(mem::bp_status());
        }
        ImGui::PopID();
      }
    }
    ImGui::EndChild();
  }

  // ── 2 补丁 ──
  if (sub == 2) {
    ImGui::TextColored(theme::muted, "补丁 · 编辑地址 %s", ui.edit_addr);
    draw_ime_field(ui, "phex", "Hex 字节", ui.patch_hex_buf,
                   sizeof(ui.patch_hex_buf), "补丁字节", "hex", dt);
    draw_ime_field(ui, "pasm", "汇编 (nop / ret / mov x0,#0)", ui.patch_asm_buf,
                   sizeof(ui.patch_asm_buf), "汇编", "text", dt);
    if (anim_button("pnop", "NOP×1", ImVec2(half, 0), ui.btns[bi++], dt,
                    theme::warn, mem::is_attached())) {
      uintptr_t a = 0;
      if (mem::parse_addr(ui.edit_addr, a)) mem::patch_nop(a, 1);
      ui.toast(mem::bp_status());
    }
    ImGui::SameLine();
    if (anim_button("pnop4", "NOP×4", ImVec2(half, 0), ui.btns[bi++], dt,
                    theme::warn, mem::is_attached())) {
      uintptr_t a = 0;
      if (mem::parse_addr(ui.edit_addr, a)) mem::patch_nop(a, 4);
      ui.toast(mem::bp_status());
    }
    if (anim_button("phexw", "写 Hex", ImVec2(half, 0), ui.btns[bi++], dt,
                    theme::danger, mem::is_attached())) {
      uintptr_t a = 0;
      if (mem::parse_addr(ui.edit_addr, a) &&
          mem::patch_hex(a, ui.patch_hex_buf))
        ui.toast(mem::bp_status());
      else ui.toast("补丁失败");
    }
    ImGui::SameLine();
    if (anim_button("pasmw", "汇编写回", ImVec2(half, 0), ui.btns[bi++], dt,
                    theme::accent, mem::is_attached())) {
      uintptr_t a = 0;
      if (mem::parse_addr(ui.edit_addr, a) &&
          mem::patch_asm(a, ui.patch_asm_buf)) {
        ui.toast(mem::bp_status());
        char hx[32];
        if (mem::assemble_to_hex(ui.patch_asm_buf, hx, sizeof(hx)))
          std::snprintf(ui.patch_hex_buf, sizeof(ui.patch_hex_buf), "%s", hx);
      } else {
        uint32_t w = 0;
        char e[96];
        ui.toast(mem::assemble_line(ui.patch_asm_buf, w, e, sizeof(e))
                     ? "写失败"
                     : (e[0] ? e : "汇编失败"));
      }
    }
    if (anim_button("pasmp", "试汇编→Hex", ImVec2(-1, 0), ui.btns[bi++], dt,
                    theme::teal, true)) {
      char hx[64];
      uint32_t w = 0;
      char e[96];
      if (mem::assemble_line(ui.patch_asm_buf, w, e, sizeof(e)) &&
          mem::assemble_to_hex(ui.patch_asm_buf, hx, sizeof(hx))) {
        std::snprintf(ui.patch_hex_buf, sizeof(ui.patch_hex_buf), "%s", hx);
        ui.toast(hx);
      } else ui.toast(e[0] ? e : "失败");
    }
  }

  // ── 3 注入 ──
  if (sub == 3) {
    ImGui::TextColored(theme::muted, "注入 / remote call");
    ImGui::TextWrapped(
        "风险：错误地址可能使目标崩溃。请先「自动找 dlopen」，"
        "so 须目标进程可读。remote call 会短暂暂停目标。");
    draw_ime_field(ui, "rfn", "函数地址", ui.remote_fn_buf,
                   sizeof(ui.remote_fn_buf), "fn", "hex", dt);
    if (anim_button("rcall", "远程 call (无参)", ImVec2(-1, 0), ui.btns[bi++],
                    dt, theme::danger, mem::is_attached())) {
      uintptr_t fn = 0;
      if (mem::parse_addr(ui.remote_fn_buf, fn) && fn) {
        uint64_t ret = 0;
        if (mem::remote_call(fn, nullptr, 0, &ret)) {
          char b[64];
          std::snprintf(b, sizeof(b), "ret=0x%llx", (unsigned long long)ret);
          ui.toast(b);
        } else ui.toast(mem::bp_status());
      } else ui.toast("函数地址无效");
    }
    draw_ime_field(ui, "dlopen", "dlopen 地址(0=自动)", ui.dlopen_addr_buf,
                   sizeof(ui.dlopen_addr_buf), "dlopen", "hex", dt);
    if (anim_button("fdlo", "自动找 dlopen", ImVec2(half, 0), ui.btns[bi++],
                    dt, theme::teal, mem::is_attached())) {
      if (mem::sym_count() == 0) mem::sym_refresh();
      uintptr_t a = 0;
      if (mem::find_dlopen(a)) {
        std::snprintf(ui.dlopen_addr_buf, sizeof(ui.dlopen_addr_buf), "0x%llX",
                      (unsigned long long)a);
        ui.toast(mem::bp_status());
      } else ui.toast(mem::bp_status());
    }
    ImGui::SameLine();
    if (anim_button("symr2", "刷新符号", ImVec2(half, 0), ui.btns[bi++], dt,
                    theme::secondary, mem::is_attached())) {
      int n = mem::sym_refresh();
      char b[48];
      std::snprintf(b, sizeof(b), "符号 %d", n);
      ui.toast(b);
    }
    draw_ime_field(ui, "sopath", "so 绝对路径", ui.inject_path,
                   sizeof(ui.inject_path), "so", "text", dt);
    ImGui::TextDisabled("例: /data/local/tmp/libxxx.so");
    if (anim_button("inso", "注入 so (dlopen)", ImVec2(-1, 0), ui.btns[bi++],
                    dt, theme::danger, mem::is_attached() && ui.inject_path[0])) {
      uintptr_t dlo = 0;
      if (ui.dlopen_addr_buf[0]) mem::parse_addr(ui.dlopen_addr_buf, dlo);
      uint64_t h = 0;
      if (mem::inject_so(dlo, ui.inject_path, &h)) {
        char b[72];
        std::snprintf(b, sizeof(b), "handle=0x%llx", (unsigned long long)h);
        if (!dlo) {
          uintptr_t found = 0;
          if (mem::find_dlopen(found))
            std::snprintf(ui.dlopen_addr_buf, sizeof(ui.dlopen_addr_buf),
                          "0x%llX", (unsigned long long)found);
        }
        ui.toast(b);
      } else ui.toast(mem::bp_status());
    }
  }

  // ── 4 地图 ──
  if (sub == 4) {
    ImGui::TextColored(theme::muted, "内存地图 / maps");
    if (anim_button("maps", "刷新地图", ImVec2(-1, 0), ui.btns[bi++], dt,
                    theme::accent2, mem::is_attached())) {
      ui.maps_cache = mem::load_maps(false);
      char buf[80];
      std::snprintf(buf, sizeof(buf), "映射 %d 段", (int)ui.maps_cache.size());
      ui.toast(buf);
    }
    draw_ime_field(ui, "mfilt", "过滤", ui.maps_filter, sizeof(ui.maps_filter),
                   "maps 过滤", "text", dt);
    if (ImGui::BeginChild("mapview", ImVec2(0, 0), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
      if (ui.maps_cache.empty())
        ImGui::TextDisabled("点「刷新地图」");
      for (size_t i = 0; i < ui.maps_cache.size(); ++i) {
        auto& r = ui.maps_cache[i];
        if (ui.maps_filter[0] &&
            r.path.find(ui.maps_filter) == std::string::npos &&
            !std::strstr(r.perms, ui.maps_filter))
          continue;
        ImGui::PushID((int)i);
        ImGui::Text("0x%llX-0x%llX %s", (unsigned long long)r.start,
                    (unsigned long long)r.end, r.perms);
        if (!r.path.empty()) {
          ImGui::SameLine();
          ImGui::TextDisabled("%s", r.path.c_str());
        }
        if (chip_button("浏览", theme::secondary)) {
          ui.browse_base = r.start;
          std::snprintf(ui.edit_addr, sizeof(ui.edit_addr), "0x%llX",
                        (unsigned long long)r.start);
          ui.tab = Tab::Addr;
          ui.addr_sub = 1;
          browse_reload(ui);
        }
        ImGui::PopID();
      }
    }
    ImGui::EndChild();
  }
}


// 主面板拖/缩（标题栏拖动，右下角缩放）
void update_main_panel_geom(Ui& ui, float alpha) {
  ImGuiIO& io = ImGui::GetIO();
  const bool down = io.MouseDown[0];
  const ImVec2 m = io.MousePos;
  // 边沿检测放最前：即使面板动画中也更新，避免 just_pressed 丢失
  static bool s_prev_down = false;
  const bool just_pressed = down && !s_prev_down;
  s_prev_down = down;

  if (alpha < 0.5f) {
    ui.panel_dragging = ui.panel_resizing = false;
    ui.panel_resize_mask = 0;
    return;
  }

  auto disp = vkeng::display();
  const float sw = (float)disp.width, sh = (float)disp.height;
  const float min_w = 280.f, min_h = 280.f;
  // 触摸友好：右边改宽、底边改高、右下角双轴；热区加大（底边更易拖）
  const float edge_r = 36.f;
  const float edge_b = 44.f;
  const float corner = 48.f;

  auto& p = ui.panel_pos;
  auto& s = ui.panel_size;

  auto in_title = [&](ImVec2 pt) {
    return pt.x >= p.x + 8.f && pt.x < p.x + s.x - 100.f && pt.y >= p.y &&
           pt.y < p.y + 52.f;
  };
  // bit0=宽 bit1=高
  auto hit_resize = [&](ImVec2 pt) -> int {
    const float r = p.x + s.x;
    const float b = p.y + s.y;
    // 允许稍微超出窗口边缘，方便摸到底边/右边
    if (pt.x < p.x - 4.f || pt.x >= r + 16.f || pt.y < p.y - 4.f ||
        pt.y >= b + 20.f)
      return 0;
    const bool near_r = pt.x >= r - edge_r && pt.x < r + 16.f;
    const bool near_b = pt.y >= b - edge_b && pt.y < b + 20.f;
    // 角优先（双轴）
    if ((pt.x >= r - corner && pt.y >= b - corner) || (near_r && near_b))
      return 3;
    if (near_r) return 1;  // 仅宽
    if (near_b) return 2;  // 仅高
    return 0;
  };

  if (!down) {
    ui.panel_dragging = ui.panel_resizing = false;
    ui.panel_resize_mask = 0;
  } else if (!ui.panel_dragging && !ui.panel_resizing &&
             (just_pressed || io.MouseClicked[0])) {
    int mask = hit_resize(m);
    if (mask) {
      ui.panel_resizing = true;
      ui.panel_resize_mask = mask;
      ui.panel_grab_m = m;
      ui.panel_grab_size = s;
      ui.panel_grab_pos = p;
    } else if (in_title(m) && !soft_ime::is_open()) {
      ui.panel_dragging = true;
      ui.panel_grab_m = m;
      ui.panel_grab_pos = p;
    }
  }

  if (ui.panel_dragging && down) {
    p.x = ui.panel_grab_pos.x + (m.x - ui.panel_grab_m.x);
    p.y = ui.panel_grab_pos.y + (m.y - ui.panel_grab_m.y);
    p.x = clampf(p.x, 0.f, std::max(0.f, sw - 80.f));
    p.y = clampf(p.y, 0.f, std::max(0.f, sh - 80.f));
  } else if (ui.panel_resizing && down) {
    float max_w = std::max(min_w, sw - p.x - 4.f);
    float max_h = std::max(min_h, sh - p.y - 4.f);
    if (ui.panel_resize_mask & 1) {
      s.x = clampf(ui.panel_grab_size.x + (m.x - ui.panel_grab_m.x), min_w,
                   max_w);
    }
    if (ui.panel_resize_mask & 2) {
      s.y = clampf(ui.panel_grab_size.y + (m.y - ui.panel_grab_m.y), min_h,
                   max_h);
    }
  }
  // 缩放/拖动时吞掉鼠标，避免内容区把拖动当成滚动
  if (ui.panel_resizing || ui.panel_dragging) {
    io.WantCaptureMouse = true;
  }

  // 防御钳制（松手后也跑，保证合法）
  if (s.x < min_w) s.x = min_w;
  if (s.y < min_h) s.y = min_h;
  if (s.x > sw) s.x = sw;
  if (s.y > sh) s.y = sh;
  if (p.x + s.x > sw) p.x = std::max(0.f, sw - s.x);
  if (p.y + s.y > sh) p.y = std::max(0.f, sh - s.y);
  if (!std::isfinite(s.x) || !std::isfinite(s.y) || !std::isfinite(p.x) ||
      !std::isfinite(p.y)) {
    s.x = clampf(sw * 0.9f, min_w, 720.f);
    s.y = clampf(sh * 0.75f, min_h, 900.f);
    p.x = (sw - s.x) * 0.5f;
    p.y = (sh - s.y) * 0.5f;
  }
}

// ── 主面板 ────────────────────────────────────────────────
void draw_panel(Ui& ui, float dt) {
  const float target = ui.want_open ? 1.f : 0.f;
  ui.open_anim = lerpf(ui.open_anim, target, clampf(dt * 7.f, 0.f, 1.f));
  if (std::fabs(ui.open_anim - target) < 0.002f) ui.open_anim = target;

  if (!ui.want_open && soft_ime::is_open()) soft_ime::close();
  if (ui.open_anim < 0.01f) return;

  const float t = ease_out_back(ui.open_anim);
  const float alpha = ease_out_cubic(ui.open_anim);
  auto disp = vkeng::display();
  const float sw = (float)disp.width, sh = (float)disp.height;

  // 默认更大，方便点按；用户缩放后记住
  if (!ui.panel_geom_init) {
    ui.panel_size.x = clampf(sw * 0.92f, 320.f, 720.f);
    ui.panel_size.y = clampf(sh * 0.78f, 400.f, 900.f);
    ui.panel_pos.x = (sw - ui.panel_size.x) * 0.5f;
    ui.panel_pos.y = (sh - ui.panel_size.y) * 0.5f;
    ui.panel_geom_init = true;
  }

  // 开合动画：从球弹出到用户位置
  ImVec2 ball_c(ui.ball_pos.x + ui.ball_r, ui.ball_pos.y + ui.ball_r);
  ImVec2 anim_pos = ui.panel_pos;
  ImVec2 anim_size = ui.panel_size;
  if (t < 0.999f && !ui.panel_dragging && !ui.panel_resizing) {
    ImVec2 start(ball_c.x - ui.panel_size.x * 0.12f,
                 ball_c.y - ui.panel_size.y * 0.08f);
    anim_pos.x = lerpf(start.x, ui.panel_pos.x, t);
    anim_pos.y = lerpf(start.y, ui.panel_pos.y, t);
    anim_size.x = ui.panel_size.x * lerpf(0.4f, 1.f, t);
    anim_size.y = ui.panel_size.y * lerpf(0.4f, 1.f, t);
  }

  update_main_panel_geom(ui, alpha);
  // 拖缩时用实时几何
  if (ui.panel_dragging || ui.panel_resizing || t >= 0.999f) {
    anim_pos = ui.panel_pos;
    anim_size = ui.panel_size;
  }

  // 强制窗口尺寸：禁止 ImGui 按内容撑开高度（否则只能感觉到左右缩放）
  ImGui::SetNextWindowPos(anim_pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(anim_size, ImGuiCond_Always);
  ImGui::SetNextWindowSizeConstraints(ImVec2(280.f, 280.f),
                                      ImVec2(sw, sh));
  ImGui::SetNextWindowBgAlpha(0.0f);

  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 22.f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 14.f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 10));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 0.7f, 0.8f, 0.45f * alpha));
  ImGui::PushStyleColor(ImGuiCol_Text, theme::text);
  ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(1.f, 0.75f, 0.82f, 0.55f));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                        ImVec4(1.f, 0.65f, 0.78f, 0.65f));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.f, 0.94f, 0.96f, 0.95f));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1.f, 0.97f, 0.98f, 0.75f));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(1.f, 0.9f, 0.93f, 0.4f));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(1.f, 0.65f, 0.78f, 0.7f));

  ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoScrollWithMouse;

  bool open = true;
  if (ImGui::Begin("##MemDbg", &open, flags)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    ImVec2 ws = ImGui::GetWindowSize();
    // 尺寸以我们维护的 panel_size 为准，不从 ImGui 回写（避免内容撑高回弹）
    if (!ui.panel_dragging && !ui.panel_resizing && t >= 0.999f) {
      // 仅位置允许极小同步
      ui.panel_pos = wp;
    }

    const float rnd = 22.f;
    // 外层软阴影
    dl->AddRectFilled(ImVec2(wp.x + 4, wp.y + 6),
                      ImVec2(wp.x + ws.x + 4, wp.y + ws.y + 6),
                      col4(0.95f, 0.55f, 0.7f, 0.22f * alpha), rnd);
    // 奶油粉底
    dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y),
                      col4(theme::bg_deep.x, theme::bg_deep.y, theme::bg_deep.z,
                           theme::bg_deep.w * alpha),
                      rnd);
    // 顶栏粉彩渐变
    dl->AddRectFilledMultiColor(
        wp, ImVec2(wp.x + ws.x, wp.y + 58.f),
        col4(1.f, 0.72f, 0.82f, 0.75f * alpha),
        col4(0.85f, 0.72f, 1.f, 0.65f * alpha),
        col4(1.f, 0.96f, 0.97f, 0.f), col4(1.f, 0.96f, 0.97f, 0.f));
    // 粉边框
    dl->AddRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y),
                col4(1.f, 0.6f, 0.75f, 0.55f * alpha), rnd, 0, 2.f);

    // 装饰爱心
    draw_heart(dl, ImVec2(wp.x + 28.f, wp.y + 22.f), 9.f,
               col4(1.f, 0.55f, 0.7f, 0.9f * alpha));
    draw_heart(dl, ImVec2(wp.x + ws.x - 48.f, wp.y + 18.f), 7.f,
               col4(0.85f, 0.7f, 1.f, 0.75f * alpha));

    // 标题（可拖）
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.f);
    ImGui::TextColored(theme::accent, "  MemDbg");
    ImGui::SameLine();
    if (mem::is_attached()) {
      ImGui::TextColored(theme::success, "· %d", mem::attached_pid());
      ImGui::SameLine();
      ImGui::TextColored(theme::muted, "%.18s", mem::attached_name());
    } else {
      ImGui::TextColored(theme::warn, "· 未附加");
    }
    ImGui::SameLine(ws.x - 110.f);
    {
      static AnimBtn close_btn{};
      if (anim_button("fold", "收起", ImVec2(88, 40), close_btn, dt,
                      theme::bubble)) {
        ui.want_open = false;
        ui.toast("面板收起啦～");
      }
    }

    ImGui::PushStyleColor(ImGuiCol_Text, theme::muted);
    ImGui::TextWrapped(
        "%s", ui.status[0] ? ui.status
                           : "拖标题移动 · 右边/底边/右下角缩放 · 内容可滚动");
    ImGui::PopStyleColor();

    ImGui::Spacing();

    // 主导航 6 项：2×3 网格，CE 工作流一屏可见
    int bi = 0;
    const int ntab = (int)Tab::COUNT;
    const int cols = 3;
    const float gap = 6.f;
    const float row_h = theme::btn_min_h * 0.92f;
    const float cell_w =
        (ImGui::GetContentRegionAvail().x - gap * (float)(cols - 1)) /
        (float)cols;

    for (int i = 0; i < ntab; ++i) {
      if (i % cols) ImGui::SameLine(0, gap);
      bool sel = (int)ui.tab == i;
      ImVec4 c = sel ? theme::accent : theme::tab_idle;
      if (anim_button(kTabNames[i], kTabNames[i], ImVec2(cell_w, row_h),
                      ui.btns[bi++], dt, c)) {
        if ((int)ui.tab != i) {
          ui.tab_prev = ui.tab;
          ui.tab = (Tab)i;
          ui.tab_anim = 0.f;
        }
      }
    }
    ui.tab_anim = lerpf(ui.tab_anim, 1.f, clampf(dt * 10.f, 0.f, 1.f));

    // 上下文条：附加状态 + 当前地址（各页共用，减少来回翻）
    {
      ImGui::Spacing();
      if (mem::is_attached()) {
        ImGui::TextColored(theme::success, "● %d %s", mem::attached_pid(),
                           mem::attached_name());
      } else {
        ImGui::TextColored(theme::warn, "○ 未附加");
      }
      ImGui::SameLine();
      ImGui::TextDisabled("  地址");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
      // 只读展示；点进地址页再改
      ImGui::TextColored(theme::peach, "%s", ui.edit_addr);
    }

    ImGui::Spacing();
    {
      ImVec2 a = ImGui::GetCursorScreenPos();
      dl->AddRectFilled(a, ImVec2(a.x + ws.x - 32.f, a.y + 3.f),
                        col4(1.f, 0.7f, 0.8f, 0.55f * alpha), 2.f);
      ImGui::Dummy(ImVec2(1, 6));
    }

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha * ease_out_cubic(ui.tab_anim));
    // 内容区高度 = 窗口剩余空间（扣底部缩放条），超出则纵向滚动
    // 显式像素高度，避免 ImGui 负高度在极小窗口失效
    const float grip_bar = 22.f;
    float body_h = ImGui::GetContentRegionAvail().y - grip_bar;
    if (body_h < 48.f) body_h = 48.f;
    if (ImGui::BeginChild(
            "tab_body", ImVec2(0, body_h), ImGuiChildFlags_None,
            ImGuiWindowFlags_AlwaysVerticalScrollbar |
                ImGuiWindowFlags_HorizontalScrollbar)) {
      switch (ui.tab) {
        case Tab::Process:
          tab_process(ui, dt, bi);
          break;
        case Tab::Scan:
          tab_scan(ui, dt, bi);
          break;
        case Tab::Addr:
          tab_addr(ui, dt, bi);
          break;
        case Tab::Analyze:
          tab_analyze(ui, dt, bi);
          break;
        case Tab::Debug:
          tab_debug(ui, dt, bi);
          break;
        case Tab::Auto:
          tab_auto(ui, dt, bi);
          break;
        default:
          break;
      }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // 缩放把手：底边条 + 右边条 + 右下角（示意可上下/左右拖）
    {
      const float edge_vis = 6.f;
      // 底边
      dl->AddRectFilled(ImVec2(wp.x + 18.f, wp.y + ws.y - edge_vis - 2.f),
                        ImVec2(wp.x + ws.x - 36.f, wp.y + ws.y - 2.f),
                        col4(1.f, 0.65f, 0.78f, 0.45f * alpha), 3.f);
      // 右边
      dl->AddRectFilled(ImVec2(wp.x + ws.x - edge_vis - 2.f, wp.y + 60.f),
                        ImVec2(wp.x + ws.x - 2.f, wp.y + ws.y - 36.f),
                        col4(1.f, 0.65f, 0.78f, 0.45f * alpha), 3.f);
      // 右下角大把手
      ImVec2 b(wp.x + ws.x - 4.f, wp.y + ws.y - 4.f);
      ImVec2 a(b.x - 28.f, b.y - 28.f);
      dl->AddRectFilled(a, b, col4(1.f, 0.65f, 0.78f, 0.9f * alpha), 10.f);
      dl->AddTriangleFilled(ImVec2(b.x - 4.f, a.y + 6.f),
                            ImVec2(b.x - 4.f, b.y - 4.f),
                            ImVec2(a.x + 6.f, b.y - 4.f),
                            col4(1.f, 0.95f, 0.97f, 0.95f * alpha));
      draw_heart(dl, ImVec2(a.x + 11.f, a.y + 11.f), 5.f,
                 col4(1.f, 0.45f, 0.65f, 0.9f * alpha));
    }

    // Toast（奶油气泡）
    if (ui.toast_t > 0.01f) {
      ui.toast_t = lerpf(ui.toast_t, 0.f, clampf(dt * 0.7f, 0.f, 1.f));
      float ta = ease_out_cubic(std::min(ui.toast_t * 2.f, 1.f)) *
                 (ui.toast_t > 0.3f ? 1.f : ui.toast_t / 0.3f);
      ImVec2 ts = ImGui::CalcTextSize(ui.toast_msg);
      ImVec2 tp(wp.x + (ws.x - ts.x) * 0.5f - 18.f,
                wp.y + ws.y - 56.f - (1.f - ta) * 20.f);
      dl->AddRectFilled(tp, ImVec2(tp.x + ts.x + 36.f, tp.y + ts.y + 18.f),
                        col4(1.f, 0.85f, 0.9f, 0.92f * ta), 16.f);
      dl->AddRect(tp, ImVec2(tp.x + ts.x + 36.f, tp.y + ts.y + 18.f),
                  col4(1.f, 0.55f, 0.72f, 0.7f * ta), 16.f, 0, 2.f);
      dl->AddText(ImVec2(tp.x + 18.f, tp.y + 9.f),
                  col4(theme::text.x, theme::text.y, theme::text.z, 0.95f * ta),
                  ui.toast_msg);
    }
  }
  ImGui::End();

  if (!open) ui.want_open = false;

  ImGui::PopStyleColor(9);
  ImGui::PopStyleVar(5);
}

}  // namespace

static void apply_kawaii_imgui_style() {
  ImGuiStyle& st = ImGui::GetStyle();
  st.FramePadding = ImVec2(14, 12);   // 更大触控
  st.ItemSpacing = ImVec2(12, 12);
  st.WindowRounding = 18.f;
  st.FrameRounding = 14.f;
  st.GrabRounding = 12.f;
  st.ScrollbarSize = 22.f;
  st.TouchExtraPadding = ImVec2(6, 6);

  ImVec4* c = st.Colors;
  // 深色字 + 粉底，避免按钮看不见字
  c[ImGuiCol_Text] = ImVec4(0.28f, 0.18f, 0.24f, 1.f);  // 深色字保证可见
  c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.42f, 0.48f, 1.f);
  c[ImGuiCol_WindowBg] = ImVec4(1.f, 0.96f, 0.97f, 0.97f);
  c[ImGuiCol_ChildBg] = ImVec4(1.f, 0.97f, 0.98f, 0.80f);
  c[ImGuiCol_PopupBg] = ImVec4(1.f, 0.95f, 0.97f, 0.98f);
  c[ImGuiCol_Border] = ImVec4(1.f, 0.70f, 0.80f, 0.55f);
  c[ImGuiCol_FrameBg] = ImVec4(1.f, 0.92f, 0.94f, 1.f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(1.f, 0.85f, 0.90f, 1.f);
  c[ImGuiCol_FrameBgActive] = ImVec4(1.f, 0.78f, 0.86f, 1.f);
  c[ImGuiCol_TitleBg] = ImVec4(1.f, 0.75f, 0.85f, 1.f);
  c[ImGuiCol_TitleBgActive] = ImVec4(1.f, 0.65f, 0.80f, 1.f);
  c[ImGuiCol_Button] = ImVec4(1.f, 0.78f, 0.86f, 1.f);
  c[ImGuiCol_ButtonHovered] = ImVec4(1.f, 0.68f, 0.80f, 1.f);
  c[ImGuiCol_ButtonActive] = ImVec4(1.f, 0.58f, 0.74f, 1.f);
  c[ImGuiCol_Header] = ImVec4(1.f, 0.80f, 0.88f, 0.85f);
  c[ImGuiCol_HeaderHovered] = ImVec4(1.f, 0.70f, 0.82f, 0.95f);
  c[ImGuiCol_HeaderActive] = ImVec4(1.f, 0.60f, 0.76f, 1.f);
  c[ImGuiCol_CheckMark] = ImVec4(0.95f, 0.35f, 0.55f, 1.f);
  c[ImGuiCol_SliderGrab] = ImVec4(1.f, 0.55f, 0.72f, 1.f);
  c[ImGuiCol_SliderGrabActive] = ImVec4(0.95f, 0.40f, 0.60f, 1.f);
  c[ImGuiCol_ScrollbarGrab] = ImVec4(1.f, 0.65f, 0.78f, 0.85f);
  c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.f, 0.55f, 0.72f, 1.f);
  c[ImGuiCol_Separator] = ImVec4(1.f, 0.75f, 0.85f, 0.7f);
  c[ImGuiCol_Tab] = ImVec4(1.f, 0.88f, 0.92f, 1.f);
  c[ImGuiCol_TabHovered] = ImVec4(1.f, 0.70f, 0.82f, 1.f);
  c[ImGuiCol_TabSelected] = ImVec4(1.f, 0.60f, 0.76f, 1.f);
}

int run(const Config& cfg) {
  // 启动时清一轮残留（保留 .mdbg_ui.cfg），避免换设备/多次运行堆垃圾
  mem::icon_cleanup_traces();
  unlink("imgui.ini");
  unlink("/data/local/tmp/imgui.ini");

  vkeng::Config vcfg;
  vcfg.layer_name = "MemDbg";
  vcfg.skip_screenshot = cfg.skip_screenshot;
  if (!vkeng::init(vcfg)) {
    std::fprintf(stderr, "Vulkan 引擎初始化失败\n");
    return 1;
  }
  apply_kawaii_imgui_style();
  // 再次确保（init 后 style 等逻辑不应恢复 ini）
  ImGui::GetIO().IniFilename = nullptr;
  ImGui::GetIO().LogFilename = nullptr;

  auto d0 = vkeng::display();
  if (!touch::init((float)d0.width, (float)d0.height)) {
    std::fprintf(stderr, "触摸初始化失败（需要 root 读 /dev/input）\n");
    vkeng::shutdown();
    return 2;
  }
  touch::set_display((float)d0.width, (float)d0.height, d0.orient);

  Ui ui;
  ui.ball_pos = ImVec2(80.f, (float)d0.height * 0.25f);
  for (int i = 0; i < 16; ++i)
    std::snprintf(ui.hex_preview[i], 3, "00");
  load_ui_config(ui);  // 恢复上次面板/过滤/子页
  refresh_process_list(ui);

  // 仅当显式 --pid N 时自动附加（不再回退 system_server/init）
  if (cfg.debug_pid > 0) {
    if (mem::attach(cfg.debug_pid)) {
      ui.attached_pid = cfg.debug_pid;
      std::snprintf(ui.attached_name, sizeof(ui.attached_name), "%s",
                    mem::attached_name());
      char buf[96];
      std::snprintf(buf, sizeof(buf), "已附加 PID %d", cfg.debug_pid);
      ui.toast(buf);
      std::fprintf(stderr, "[dbg] attached pid %d (%s)\n", cfg.debug_pid,
                   mem::attached_name());
    } else {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "PID %d 附加失败", cfg.debug_pid);
      ui.toast(buf);
      std::fprintf(stderr, "[dbg] attach %d failed\n", cfg.debug_pid);
    }
  }

  if (cfg.demo_ui) {
    ui.want_open = true;
    ui.toast("演示模式：自动轮播界面");
    // 若已附加：预开反汇编/伪C/断点窗，便于截图分析
    if (mem::is_attached()) {
      uintptr_t sample = 0;
      mem::DisasmOptions dop;
      dop.filter_noise = false;  // 演示时不过滤，便于看伪C覆盖
      auto maps = mem::load_maps_filtered(mem::RegionFilter::Code);
      // 扫描可执行段，找第一条能读出真实指令的地址
      for (auto& r : maps) {
        if (r.end <= r.start + 64) continue;
        // 跳过匿名极小段
        uint8_t probe[16]{};
        if (!mem::read_mem(r.start, probe, sizeof(probe))) continue;
        auto try_ins = mem::disasm_at(r.start, 16, dop);
        int ok = 0;
        for (auto& in : try_ins)
          if (in.mnem[0] && std::strcmp(in.mnem, ".word") != 0) ok++;
        if (ok >= 2) {
          sample = r.start;
          std::fprintf(stderr, "[demo] pick code map %s @0x%llx (%d decoded)\n",
                       r.path.empty() ? "(anon)" : r.path.c_str(),
                       (unsigned long long)sample, ok);
          break;
        }
      }
      if (!sample && !maps.empty()) sample = maps[0].start;
      if (!sample) sample = 0x1000;
      ui.filter_asm_noise = false;
      open_analysis(ui, sample, true);
      // 强制再开一扇伪C窗
      open_pseudo_window(ui, sample);
      spawn_float(ui, Ui::FloatKind::Breakpoints, 0);
      spawn_float(ui, Ui::FloatKind::HitLog, 0);
      // 错落布局浮窗，避免叠成一坨
      float y = 60.f;
      for (size_t i = 0; i < ui.float_wins.size(); ++i) {
        ui.float_wins[i].pos =
            ImVec2(20.f + (float)(i % 2) * 30.f, y + (float)i * 48.f);
        ui.float_wins[i].size = ImVec2(420.f, 300.f);
      }
      std::fprintf(stderr, "[demo] opened analysis @ 0x%llx wins=%zu\n",
                   (unsigned long long)sample, ui.float_wins.size());
      if (!ui.float_wins.empty()) {
        const auto& ps = ui.float_wins.front().pseudo;
        size_t n = std::min(ps.size(), (size_t)1600);
        std::fprintf(stderr, "[demo] pseudo-C sample (%zu bytes):\n%.*s\n---\n",
                     ps.size(), (int)n, ps.c_str());
        int non_word = 0, total = 0;
        for (auto& in : ui.float_wins.front().insns) {
          total++;
          if (std::strcmp(in.mnem, ".word") != 0) non_word++;
        }
        std::fprintf(stderr, "[demo] decode coverage: %d/%d non-.word\n",
                     non_word, total);
      }
    }
  }

  using clock = std::chrono::steady_clock;
  auto last_disp = clock::now();
  auto last_frame = clock::now();
  auto last_demo = clock::now();
  int demo_tab = 0;
  int last_orient = d0.orient;
  float last_w = (float)d0.width, last_h = (float)d0.height;
  bool mouse_down = false;

  while (ui.running) {
    if (ui.want_exit) {
      do_exit_cleanup(ui);
      break;
    }
    const auto t0 = clock::now();
    float dt = std::chrono::duration<float>(t0 - last_frame).count();
    last_frame = t0;
    if (dt <= 0.f || dt > 0.1f) dt = 0.016f;

    if (t0 - last_disp > std::chrono::milliseconds(800)) {
      last_disp = t0;
      vkeng::sync_display();
      auto d = vkeng::display();
      if (d.width != (int)last_w || d.height != (int)last_h ||
          d.orient != last_orient) {
        if (last_w > 1.f && last_h > 1.f) {
          ui.ball_pos.x *= (float)d.width / last_w;
          ui.ball_pos.y *= (float)d.height / last_h;
        }
        last_w = (float)d.width;
        last_h = (float)d.height;
        last_orient = d.orient;
        touch::set_display(last_w, last_h, last_orient);
      }
    }

    touch::State ts = touch::snapshot();
    vkeng::new_frame();

    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(ts.x, ts.y);
    if (ts.just_pressed) {
      io.AddMouseButtonEvent(0, true);
      mouse_down = true;
    }
    if (ts.just_released) {
      io.AddMouseButtonEvent(0, false);
      mouse_down = false;
    }
    if (ts.down && !mouse_down) {
      io.AddMouseButtonEvent(0, true);
      mouse_down = true;
    }
    if (!ts.down && mouse_down && !ts.just_released) {
      io.AddMouseButtonEvent(0, false);
      mouse_down = false;
    }

    // 演示模式：轮播 Tab 便于截图
    if (cfg.demo_ui) {
      ui.want_open = true;
      auto now = clock::now();
      if (now - last_demo >
          std::chrono::milliseconds(std::max(800, cfg.demo_tab_ms))) {
        last_demo = now;
        demo_tab = (demo_tab + 1) % (int)Tab::COUNT;
        ui.tab = (Tab)demo_tab;
        ui.tab_anim = 0.f;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "演示界面: %s", kTabNames[demo_tab]);
        ui.toast(buf);
        std::fprintf(stderr, "[demo] tab=%s\n", kTabNames[demo_tab]);
      }
    }

    // 搜索线程完成 → 同步结果
    if (ui.searching && !mem::scan_busy()) {
      ui.searching = false;
      sync_results_from_engine(ui);
      ui.toast(mem::scan_status());
      if (ui.result_count > 0) {
        ui.tab = Tab::Scan; ui.scan_sub = 1;
        ui.tab_anim = 0.f;
      }
    }
    // 冻结写回（扫描冻结 + 地址表冻结）；speedhack soft 时加倍写回
    if (mem::is_attached()) {
      int reps = 1;
      if (mem::speed_active()) {
        float m = mem::speed_get();
        if (m > 1.f) reps = (int)std::min(8.f, m + 0.5f);
      }
      for (int r = 0; r < reps; ++r) {
        mem::tick_freeze(to_mem_type(ui.vtype));
        ui.addr_table.tick_freeze();
      }
      ui.table_refresh_t += dt;
      if (ui.table_refresh_t > 0.5f) {
        ui.table_refresh_t = 0.f;
        if (ui.tab == Tab::Addr || ui.tab == Tab::Auto || ui.want_open)
          ui.addr_table.refresh_values();
      }
    }

    // 全局热键
    {
      const char* hm = mem::hotkey_poll();
      if (hm && hm[0]) ui.toast(hm);
    }

    // 周期性自动保存 UI 配置（约 3 秒）
    ui.cfg_save_acc += dt;
    if (ui.cfg_save_acc > 3.f) {
      ui.cfg_save_acc = 0.f;
      save_ui_config(ui);
    }

    // 指针扫描完成
    if (ui.ptr_scanning && !mem::ptrscan_busy()) {
      ui.ptr_scanning = false;
      mem::ptrscan_copy(ui.ptr_results, 100);
      ui.toast(mem::ptrscan_status());
    }

    // 硬件断点 + 软断点 SIGTRAP 轮询 → 暂停并弹寄存器
    {
      mem::BpHit hit{};
      bool got = false;
      if (mem::bp_ready() || mem::bp_ptrace_active()) got = mem::bp_poll(hit);
      if (!got) got = mem::dbg_poll_stop(hit);
      if (got && hit.valid) {
        ui.last_hit = hit;
        ui.has_hit = true;
        ui.toast(hit.msg);
        // 命中后读寄存器
        mem::dbg_regs_read(ui.dbg_regs);
        mem::dbg_fp_regs_read(ui.dbg_fp);
        if (ui.auto_regs_on_hit) {
          bool has_reg = false;
          for (auto& w : ui.float_wins)
            if (w.kind == Ui::FloatKind::Registers && w.open) has_reg = true;
          if (!has_reg) spawn_float(ui, Ui::FloatKind::Registers, 0);
        }
        uintptr_t a = hit.pc ? hit.pc : hit.watch_addr;
        if (a) open_analysis(ui, a, true);
        bool has_log = false;
        for (auto& w : ui.float_wins)
          if (w.kind == Ui::FloatKind::HitLog && w.open) has_log = true;
        if (!has_log) spawn_float(ui, Ui::FloatKind::HitLog, 0);
      }
    }

    ImGui::NewFrame();

    draw_float_ball(ui, dt);
    draw_panel(ui, dt);
    draw_all_float_windows(ui, dt);

    // 内嵌软键盘（最上层）
    soft_ime::update_and_draw(dt);
    if (soft_ime::just_confirmed()) {
      ui.toast("输入完成");
      ui.ime_target = nullptr;
    } else if (soft_ime::just_cancelled()) {
      ui.toast("键盘已关闭");
      ui.ime_target = nullptr;
    }

    // FPS（展开时）
    if (ui.open_anim > 0.5f) {
      ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
      ImGui::SetNextWindowBgAlpha(0.4f);
      if (ImGui::Begin("##fps", nullptr,
                       ImGuiWindowFlags_NoDecoration |
                           ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoInputs |
                           ImGuiWindowFlags_NoNav |
                           ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::Text("%.0f FPS · MemDbg", io.Framerate);
      }
      ImGui::End();
    }

    vkeng::end_frame();

    const bool busy =
        ui.want_open || ui.open_anim > 0.01f || ui.open_anim < 0.99f ||
        soft_ime::is_open() || ui.toast_t > 0.01f ||
        ts.down || ts.just_pressed || ts.just_released ||
        ImGui::IsMouseDragging(0);
    // 悬浮球始终有呼吸动画 → 至少 idle 刷新
    const int sleep_ms =
        busy ? std::max(8, cfg.frame_ms) : std::max(16, cfg.idle_ms);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             clock::now() - t0)
                             .count();
    if (elapsed < sleep_ms)
      std::this_thread::sleep_for(
          std::chrono::milliseconds(sleep_ms - elapsed));
  }

  if (!ui.want_exit) {
    // 与 do_exit_cleanup 一致：先存配置再清残留
    save_ui_config(ui);
    mem::script_shutdown();
    mem::bp_shutdown();
    mem::detach();
    mem::icon_cleanup_traces();
    unlink("imgui.ini");
    unlink("/data/local/tmp/imgui.ini");
  }
  touch::shutdown();
  vkeng::shutdown();
  return 0;
}

}  // namespace float_app
