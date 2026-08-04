#pragma once
/**
 * 进程/应用图标
 * 1) PackageManager 导出缓存（app_process IconDump → PNG，最完整）
 * 2) APK 内 ic_launcher PNG 兜底
 */
#include <cstdint>
#include <string>
#include <vector>

namespace mem {

/** 包名是否在 packages.list / 可视为有应用图标 */
bool package_is_app(const std::string& pkg);

/** 解析包名对应 base.apk 路径；失败返回空 */
std::string find_apk_path(const std::string& pkg);

/**
 * 加载应用图标为 RGBA8（优先 PM 缓存 PNG，再 APK）
 * 成功返回 true，out_rgba 大小 = w*h*4
 */
bool load_app_icon_rgba(const std::string& pkg, std::vector<uint8_t>& out_rgba,
                        int& out_w, int& out_h);

/** 刷新 packages.list / apk 索引缓存 */
void icon_cache_refresh();

/**
 * 用 app_process 跑 IconDump 导出图标到 cache 目录。
 * pkgs 为空则导出全部；timeout_ms 最长等待。
 * 返回成功写出的数量（-1=启动失败）。
 */
int icon_pm_dump(const std::vector<std::string>& pkgs = {},
                 int timeout_ms = 45000);

/** PM 图标缓存目录 */
const char* icon_pm_cache_dir();

/** 设置外部 icon_dump.dex（可选；默认用内置） */
void icon_set_dex_path(const char* path);

/** 清理图标缓存 / 临时 dex / dump 痕迹（退出时调用） */
void icon_cleanup_traces();

}  // namespace mem
