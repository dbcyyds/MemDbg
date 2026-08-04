/**
 * 应用图标：
 * 1) PackageManager 缓存 PNG（IconDump / app_process）
 * 2) APK 内 ic_launcher PNG 兜底（按需 seek 读 ZIP）
 */
#include "mem_icon.hpp"
#include "icon_dump_dex_embed.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <csignal>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <zlib.h>

namespace mem {
namespace {

std::unordered_set<std::string> g_pkgs;
std::unordered_map<std::string, std::string> g_apk_map;
bool g_idx_ok = false;

// 运行期临时目录（退出时清空）
const char* kPmCache = "/data/local/tmp/.mdbg_ic";
char g_dex_path[320] = "";  // 空=使用内置 dex
char g_runtime_dex[128] = "";

uint32_t rd_u32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
uint16_t rd_u16(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

void load_packages_list() {
  g_pkgs.clear();
  FILE* f = std::fopen("/data/system/packages.list", "r");
  if (!f) return;
  char line[512];
  while (std::fgets(line, sizeof(line), f)) {
    char pkg[192]{};
    if (std::sscanf(line, "%191s", pkg) == 1 && std::strchr(pkg, '.'))
      g_pkgs.insert(pkg);
  }
  std::fclose(f);
}

void scan_apk_dirs() {
  g_apk_map.clear();
  const char* roots[] = {"/data/app", nullptr};
  for (int ri = 0; roots[ri]; ++ri) {
    DIR* d = opendir(roots[ri]);
    if (!d) continue;
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      char sub[512];
      std::snprintf(sub, sizeof(sub), "%s/%s", roots[ri], e->d_name);
      auto try_dir = [&](const char* dir) {
        char apk[560];
        std::snprintf(apk, sizeof(apk), "%s/base.apk", dir);
        FILE* t = std::fopen(apk, "rb");
        if (!t) return;
        std::fclose(t);
        std::string path = dir;
        auto slash = path.find_last_of('/');
        std::string leaf =
            slash == std::string::npos ? path : path.substr(slash + 1);
        auto dash = leaf.find('-');
        std::string pkg =
            dash != std::string::npos ? leaf.substr(0, dash) : leaf;
        if (pkg.find('.') != std::string::npos && !g_apk_map.count(pkg))
          g_apk_map[pkg] = apk;
      };
      if (std::strncmp(e->d_name, "~~", 2) == 0) {
        DIR* d2 = opendir(sub);
        if (!d2) continue;
        while (dirent* e2 = readdir(d2)) {
          if (e2->d_name[0] == '.') continue;
          char sub2[560];
          std::snprintf(sub2, sizeof(sub2), "%s/%s", sub, e2->d_name);
          try_dir(sub2);
        }
        closedir(d2);
      } else {
        try_dir(sub);
      }
    }
    closedir(d);
  }
}

void ensure_index() {
  if (g_idx_ok) return;
  load_packages_list();
  scan_apk_dirs();
  g_idx_ok = true;
}

struct ZipHit {
  std::string name;
  uint32_t comp_size = 0;
  uint32_t uncomp_size = 0;
  uint32_t local_off = 0;
  uint16_t method = 0;
  int score = 0;
};

int icon_score(const std::string& name, uint32_t size) {
  std::string l = name;
  for (char& c : l) c = (char)std::tolower((unsigned char)c);
  bool png = l.size() > 4 && l.compare(l.size() - 4, 4, ".png") == 0;
  if (!png) return -1;
  int s = (int)(size / 64);
  if (l.find("ic_launcher") != std::string::npos)
    s += 10000;
  else if (l.find("ic_app") != std::string::npos)
    s += 8000;
  else if (l.find("launcher") != std::string::npos)
    s += 5000;
  else if (l.find("/icon") != std::string::npos ||
           l.find("_icon") != std::string::npos)
    s += 1000;
  else
    return -1;
  if (l.find("xxxhdpi") != std::string::npos)
    s += 400;
  else if (l.find("xxhdpi") != std::string::npos)
    s += 300;
  else if (l.find("xhdpi") != std::string::npos)
    s += 200;
  else if (l.find("hdpi") != std::string::npos)
    s += 100;
  if (l.find("round") != std::string::npos) s += 50;
  if (l.find("foreground") != std::string::npos) s += 80;
  return s;
}

bool fread_at(FILE* f, uint64_t off, void* buf, size_t n) {
  if (std::fseek(f, (long)off, SEEK_SET) != 0) return false;
  return std::fread(buf, 1, n, f) == n;
}

bool zip_collect_icon_candidates(FILE* f, std::vector<ZipHit>& cands) {
  cands.clear();
  if (std::fseek(f, 0, SEEK_END) != 0) return false;
  long fsz = std::ftell(f);
  if (fsz < 22) return false;

  size_t back = (size_t)std::min(fsz, (long)65557);
  std::vector<uint8_t> tail(back);
  if (!fread_at(f, (uint64_t)(fsz - (long)back), tail.data(), back))
    return false;
  size_t eocd_rel = (size_t)-1;
  for (size_t i = 0; i + 22 <= back; ++i) {
    size_t pos = back - 22 - i;
    if (tail[pos] == 0x50 && tail[pos + 1] == 0x4b && tail[pos + 2] == 0x05 &&
        tail[pos + 3] == 0x06) {
      eocd_rel = pos;
      break;
    }
  }
  if (eocd_rel == (size_t)-1) return false;
  const uint8_t* eocd = tail.data() + eocd_rel;
  uint32_t cd_size = rd_u32(eocd + 12);
  uint32_t cd_off = rd_u32(eocd + 16);
  if (cd_size == 0 || cd_size > 32u * 1024 * 1024) return false;

  std::vector<uint8_t> cd(cd_size);
  if (!fread_at(f, cd_off, cd.data(), cd_size)) return false;

  size_t p = 0;
  while (p + 46 <= cd_size) {
    if (!(cd[p] == 0x50 && cd[p + 1] == 0x4b && cd[p + 2] == 0x01 &&
          cd[p + 3] == 0x02))
      break;
    uint16_t method = rd_u16(cd.data() + p + 10);
    uint32_t comp = rd_u32(cd.data() + p + 20);
    uint32_t uncomp = rd_u32(cd.data() + p + 24);
    uint16_t nlen = rd_u16(cd.data() + p + 28);
    uint16_t elen = rd_u16(cd.data() + p + 30);
    uint16_t clen = rd_u16(cd.data() + p + 32);
    uint32_t local = rd_u32(cd.data() + p + 42);
    if (p + 46 + nlen > cd_size) break;
    std::string name((const char*)cd.data() + p + 46, nlen);
    int sc = icon_score(name, uncomp ? uncomp : comp);
    if (sc >= 0 && (method == 0 || method == 8) &&
        uncomp <= 2u * 1024 * 1024 && comp <= 2u * 1024 * 1024 &&
        (uncomp == 0 || uncomp >= 800)) {
      ZipHit h;
      h.name = std::move(name);
      h.comp_size = comp;
      h.uncomp_size = uncomp;
      h.local_off = local;
      h.method = method;
      h.score = sc;
      cands.push_back(std::move(h));
    }
    p += 46 + nlen + elen + clen;
  }
  std::sort(cands.begin(), cands.end(),
            [](const ZipHit& a, const ZipHit& b) { return a.score > b.score; });
  if (cands.size() > 12) cands.resize(12);
  return !cands.empty();
}

bool zip_extract_entry(FILE* f, const ZipHit& e, std::vector<uint8_t>& out) {
  uint8_t lh[30];
  if (!fread_at(f, e.local_off, lh, 30)) return false;
  if (!(lh[0] == 0x50 && lh[1] == 0x4b && lh[2] == 0x03 && lh[3] == 0x04))
    return false;
  uint16_t nlen = rd_u16(lh + 26);
  uint16_t elen = rd_u16(lh + 28);
  uint64_t data_off = (uint64_t)e.local_off + 30 + nlen + elen;
  std::vector<uint8_t> src(e.comp_size);
  if (!fread_at(f, data_off, src.data(), e.comp_size)) return false;

  if (e.method == 0) {
    out = std::move(src);
    return true;
  }
  // deflate
  size_t want = e.uncomp_size ? e.uncomp_size : e.comp_size * 4 + 256;
  out.resize(want);
  z_stream zs{};
  zs.next_in = src.data();
  zs.avail_in = e.comp_size;
  zs.next_out = out.data();
  zs.avail_out = (uInt)out.size();
  if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
  int r = inflate(&zs, Z_FINISH);
  size_t got = zs.total_out;
  inflateEnd(&zs);
  if (r != Z_STREAM_END && r != Z_OK) return false;
  out.resize(got);
  return true;
}

// ── PNG decode 8-bit RGB/RGBA/Gray ────────────────────────
uint32_t png_be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
         (uint32_t)p[3];
}

bool png_decode(const std::vector<uint8_t>& png, std::vector<uint8_t>& rgba,
                int& w, int& h) {
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  if (png.size() < 33 || std::memcmp(png.data(), sig, 8) != 0) return false;
  size_t p = 8;
  uint32_t width = 0, height = 0;
  uint8_t bit_depth = 0, color_type = 0;
  std::vector<uint8_t> idat;
  while (p + 12 <= png.size()) {
    uint32_t len = png_be32(png.data() + p);
    const char* type = (const char*)png.data() + p + 4;
    if (p + 12 + len > png.size()) break;
    const uint8_t* chunk = png.data() + p + 8;
    if (std::strncmp(type, "IHDR", 4) == 0 && len >= 13) {
      width = png_be32(chunk);
      height = png_be32(chunk + 4);
      bit_depth = chunk[8];
      color_type = chunk[9];
    } else if (std::strncmp(type, "IDAT", 4) == 0) {
      idat.insert(idat.end(), chunk, chunk + len);
    } else if (std::strncmp(type, "IEND", 4) == 0) {
      break;
    }
    p += 12 + len;
  }
  if (!width || !height || width > 1024 || height > 1024) return false;
  if (bit_depth != 8) return false;
  if (color_type != 2 && color_type != 6 && color_type != 0 && color_type != 4)
    return false;
  int bpp =
      (color_type == 2) ? 3 : (color_type == 6) ? 4 : (color_type == 0) ? 1 : 2;
  size_t raw_need = (size_t)(1 + width * bpp) * height;
  std::vector<uint8_t> raw(raw_need + 256);
  z_stream zs{};
  zs.next_in = idat.data();
  zs.avail_in = (uInt)idat.size();
  zs.next_out = raw.data();
  zs.avail_out = (uInt)raw.size();
  if (inflateInit(&zs) != Z_OK) return false;
  int ir = inflate(&zs, Z_FINISH);
  size_t got = zs.total_out;
  inflateEnd(&zs);
  if (ir != Z_STREAM_END && ir != Z_OK) return false;
  if (got < raw_need) return false;

  std::vector<uint8_t> img((size_t)width * height * bpp);
  size_t stride = (size_t)width * bpp;
  const uint8_t* src = raw.data();
  uint8_t* dst = img.data();
  auto paeth = [](int a, int b, int c) {
    int p = a + b - c;
    int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
  };
  for (uint32_t y = 0; y < height; ++y) {
    uint8_t filter = *src++;
    uint8_t* row = dst + y * stride;
    const uint8_t* prev = y ? dst + (y - 1) * stride : nullptr;
    if (filter == 0) {
      std::memcpy(row, src, stride);
    } else {
      for (size_t x = 0; x < stride; ++x) {
        uint8_t cur = src[x];
        uint8_t a = x >= (size_t)bpp ? row[x - bpp] : 0;
        uint8_t b = prev ? prev[x] : 0;
        uint8_t c = (prev && x >= (size_t)bpp) ? prev[x - bpp] : 0;
        if (filter == 1)
          row[x] = (uint8_t)(cur + a);
        else if (filter == 2)
          row[x] = (uint8_t)(cur + b);
        else if (filter == 3)
          row[x] = (uint8_t)(cur + ((a + b) >> 1));
        else if (filter == 4)
          row[x] = (uint8_t)(cur + paeth(a, b, c));
        else
          row[x] = cur;
      }
    }
    src += stride;
  }

  rgba.resize((size_t)width * height * 4);
  for (uint32_t i = 0; i < width * height; ++i) {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    if (color_type == 2) {
      r = img[i * 3];
      g = img[i * 3 + 1];
      b = img[i * 3 + 2];
    } else if (color_type == 6) {
      r = img[i * 4];
      g = img[i * 4 + 1];
      b = img[i * 4 + 2];
      a = img[i * 4 + 3];
    } else if (color_type == 0) {
      r = g = b = img[i];
    } else {
      r = g = b = img[i * 2];
      a = img[i * 2 + 1];
    }
    rgba[i * 4 + 0] = r;
    rgba[i * 4 + 1] = g;
    rgba[i * 4 + 2] = b;
    rgba[i * 4 + 3] = a;
  }
  w = (int)width;
  h = (int)height;
  return true;
}

bool extract_icon_from_apk(const std::string& apk, std::vector<uint8_t>& rgba,
                           int& w, int& h) {
  FILE* f = std::fopen(apk.c_str(), "rb");
  if (!f) return false;
  std::vector<ZipHit> cands;
  if (!zip_collect_icon_candidates(f, cands)) {
    std::fclose(f);
    return false;
  }
  for (auto& hit : cands) {
    std::vector<uint8_t> file;
    if (!zip_extract_entry(f, hit, file) || file.size() < 24) continue;
    if (!(file[0] == 137 && file[1] == 'P')) continue;
    std::vector<uint8_t> tmp;
    int tw = 0, th = 0;
    if (!png_decode(file, tmp, tw, th)) continue;
    // 过小/畸形/极端长宽比跳过
    if (tw < 36 || th < 36) continue;
    if (tw > 1024 || th > 1024) continue;
    int mn = tw < th ? tw : th, mx = tw > th ? tw : th;
    if (mx > mn * 3) continue;  // 非方图标（广告条等）
    rgba = std::move(tmp);
    w = tw;
    h = th;
    std::fclose(f);
    return true;
  }
  std::fclose(f);
  return false;
}

}  // namespace

void icon_cache_refresh() {
  g_idx_ok = false;
  ensure_index();
}

bool package_is_app(const std::string& pkg) {
  if (pkg.empty() || pkg.find('.') == std::string::npos) return false;
  ensure_index();
  if (g_pkgs.count(pkg)) return true;
  if (g_apk_map.count(pkg)) return true;
  if (pkg.rfind("com.", 0) == 0 || pkg.rfind("org.", 0) == 0 ||
      pkg.rfind("net.", 0) == 0 || pkg.rfind("io.", 0) == 0 ||
      pkg.rfind("cn.", 0) == 0 || pkg.rfind("tv.", 0) == 0 ||
      pkg.rfind("android.", 0) == 0 || pkg.rfind("bin.", 0) == 0)
    return true;
  return false;
}

std::string find_apk_path(const std::string& pkg) {
  if (pkg.empty()) return {};
  ensure_index();
  auto it = g_apk_map.find(pkg);
  if (it != g_apk_map.end()) return it->second;
  DIR* d = opendir("/data/app");
  if (!d) return {};
  std::string found;
  while (dirent* e = readdir(d)) {
    if (std::strncmp(e->d_name, "~~", 2) != 0) continue;
    char sub[400];
    std::snprintf(sub, sizeof(sub), "/data/app/%s", e->d_name);
    DIR* d2 = opendir(sub);
    if (!d2) continue;
    while (dirent* e2 = readdir(d2)) {
      if (e2->d_name[0] == '.') continue;
      if (std::strncmp(e2->d_name, pkg.c_str(), pkg.size()) != 0) continue;
      if (e2->d_name[pkg.size()] != '-' && e2->d_name[pkg.size()] != 0)
        continue;
      char apk[480];
      std::snprintf(apk, sizeof(apk), "%s/%s/base.apk", sub, e2->d_name);
      if (FILE* t = std::fopen(apk, "rb")) {
        std::fclose(t);
        found = apk;
        g_apk_map[pkg] = found;
        break;
      }
    }
    closedir(d2);
    if (!found.empty()) break;
  }
  closedir(d);
  return found;
}

/** 从磁盘 PNG 文件解码 */
bool load_png_file(const char* path, std::vector<uint8_t>& rgba, int& w,
                   int& h) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  if (std::fseek(f, 0, SEEK_END) != 0) {
    std::fclose(f);
    return false;
  }
  long sz = std::ftell(f);
  if (sz < 32 || sz > 4 * 1024 * 1024) {
    std::fclose(f);
    return false;
  }
  std::rewind(f);
  std::vector<uint8_t> buf((size_t)sz);
  if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
    std::fclose(f);
    return false;
  }
  std::fclose(f);
  return png_decode(buf, rgba, w, h);
}

bool load_app_icon_rgba(const std::string& pkg, std::vector<uint8_t>& out_rgba,
                        int& out_w, int& out_h) {
  out_rgba.clear();
  out_w = out_h = 0;
  if (pkg.empty()) return false;

  // 1) PackageManager 导出缓存（最完整：自适应图标/WebP/主题）
  char cache_png[400];
  std::snprintf(cache_png, sizeof(cache_png), "%s/%s.png", kPmCache,
                pkg.c_str());
  if (load_png_file(cache_png, out_rgba, out_w, out_h)) return true;

  // 2) APK 内 PNG 兜底
  std::string apk = find_apk_path(pkg);
  if (apk.empty()) return false;
  return extract_icon_from_apk(apk, out_rgba, out_w, out_h);
}

const char* icon_pm_cache_dir() { return kPmCache; }

void icon_set_dex_path(const char* path) {
  if (path && path[0])
    std::snprintf(g_dex_path, sizeof(g_dex_path), "%s", path);
}

static bool file_ok(const char* p) {
  struct stat st {};
  return stat(p, &st) == 0 && st.st_size > 100;
}

/** 将内置 dex 写到临时文件（随机名，用后删除） */
static const char* extract_embedded_dex() {
  if (g_runtime_dex[0] && file_ok(g_runtime_dex)) return g_runtime_dex;
  // 优先用户指定外部路径
  if (g_dex_path[0] && file_ok(g_dex_path)) return g_dex_path;

  std::snprintf(g_runtime_dex, sizeof(g_runtime_dex),
                "/data/local/tmp/.mdbg_%d.dex", (int)getpid());
  FILE* out = std::fopen(g_runtime_dex, "wb");
  if (!out) {
    std::snprintf(g_runtime_dex, sizeof(g_runtime_dex),
                  "/data/local/tmp/.mdbg_ic.dex");
    out = std::fopen(g_runtime_dex, "wb");
  }
  if (!out) {
    g_runtime_dex[0] = 0;
    return nullptr;
  }
  size_t w =
      std::fwrite(k_icon_dump_dex, 1, k_icon_dump_dex_len, out);
  std::fclose(out);
  chmod(g_runtime_dex, 0600);
  if (w != k_icon_dump_dex_len) {
    unlink(g_runtime_dex);
    g_runtime_dex[0] = 0;
    return nullptr;
  }
  return g_runtime_dex;
}

/** 是否应保留的配置文件（不自动删除） */
static bool is_kept_config_name(const char* name) {
  if (!name || !name[0]) return false;
  // UI 面板几何/过滤等
  if (std::strcmp(name, ".mdbg_ui.cfg") == 0) return true;
  // 若用户显式启用过 ImGui ini（默认已禁用），也不误删
  if (std::strcmp(name, "imgui.ini") == 0) return true;
  if (std::strcmp(name, ".imgui.ini") == 0) return true;
  return false;
}

void icon_cleanup_traces() {
  // 删除运行期 dex
  if (g_runtime_dex[0]) {
    unlink(g_runtime_dex);
    g_runtime_dex[0] = 0;
  }
  unlink("/data/local/tmp/.mdbg_ic.dex");
  unlink("/data/local/tmp/icon_dump.dex");
  unlink("/data/local/tmp/memdbg_icondump.log");
  // 清空图标缓存目录
  DIR* d = opendir(kPmCache);
  if (d) {
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      char path[400];
      std::snprintf(path, sizeof(path), "%s/%s", kPmCache, e->d_name);
      unlink(path);
    }
    closedir(d);
  }
  rmdir(kPmCache);
  // 清理历史 memdbg / 打包缓存 / 运行残留；保留 UI 配置
  d = opendir("/data/local/tmp");
  if (d) {
    while (dirent* e = readdir(d)) {
      const char* n = e->d_name;
      if (is_kept_config_name(n)) continue;
      bool drop = false;
      if (std::strncmp(n, "memdbg_", 7) == 0) drop = true;
      else if (std::strncmp(n, ".mdbg_", 6) == 0) drop = true;
      else if (std::strncmp(n, ".mdbg", 5) == 0) drop = true;
      // 打包脚本缓存：.m_<tag> / .mxxxxxx
      else if (n[0] == '.' && n[1] == 'm' && n[2] == '_') drop = true;
      else if (std::strncmp(n, ".x", 2) == 0) {
        // mktemp 临时目录名 .xXXXXXX —— 只删空壳文件，目录另处理
      }
      if (!drop) continue;
      char path[400];
      std::snprintf(path, sizeof(path), "/data/local/tmp/%s", n);
      struct stat st {};
      if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) unlink(path);
    }
    closedir(d);
  }
  // 旧图标目录
  d = opendir("/data/local/tmp/memdbg_icons");
  if (d) {
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      char path[420];
      std::snprintf(path, sizeof(path), "/data/local/tmp/memdbg_icons/%s",
                    e->d_name);
      unlink(path);
    }
    closedir(d);
    rmdir("/data/local/tmp/memdbg_icons");
  }
}

int icon_pm_dump(const std::vector<std::string>& pkgs, int timeout_ms) {
  const char* dex = extract_embedded_dex();
  if (!dex) return -1;

  mkdir(kPmCache, 0700);

  std::vector<std::string> args_store;
  args_store.push_back("app_process64");
  args_store.push_back("/system/bin");
  args_store.push_back("IconDump");
  args_store.push_back(kPmCache);
  for (auto& p : pkgs) args_store.push_back(p);

  std::vector<char*> argv;
  for (auto& s : args_store) argv.push_back(s.data());
  argv.push_back(nullptr);

  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    setenv("CLASSPATH", dex, 1);
    // 不落盘日志，丢弃输出
    freopen("/dev/null", "w", stderr);
    freopen("/dev/null", "w", stdout);
    execvp("app_process64", argv.data());
    execvp("app_process", argv.data());
    execv("/system/bin/app_process64", argv.data());
    execv("/system/bin/app_process", argv.data());
    _exit(127);
  }

  int waited = 0;
  int status = 0;
  while (waited < timeout_ms) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) break;
    usleep(100000);
    waited += 100;
  }
  if (waited >= timeout_ms) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
  }

  // 用完即删运行 dex（下次再解压）
  if (g_runtime_dex[0] && std::strcmp(dex, g_runtime_dex) == 0) {
    unlink(g_runtime_dex);
    // 保留路径缓冲以便复用名，但标记无效
    // 下次 extract 会重写
  }

  int n = 0;
  if (pkgs.empty()) {
    DIR* d = opendir(kPmCache);
    if (d) {
      while (dirent* e = readdir(d)) {
        if (std::strstr(e->d_name, ".png")) n++;
      }
      closedir(d);
    }
  } else {
    for (auto& p : pkgs) {
      char path[400];
      std::snprintf(path, sizeof(path), "%s/%s.png", kPmCache, p.c_str());
      if (file_ok(path)) n++;
    }
  }
  return n;
}

}  // namespace mem
