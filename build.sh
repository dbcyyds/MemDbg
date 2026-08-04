#!/usr/bin/env bash
# MemDbg — 单文件构建（无系统输入法 / 无宿主 APK）
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
SYS_LIB="${SYS_LIB:-/system/lib64}"
NDKLIB="${NDKLIB:-$PREFIX/aarch64-linux-android/lib}"
OUT="${ROOT}/out"
OBJ="${OUT}/obj"
# 优先仓库内 third_party/imgui；兼容旧符号链接 third_party_imgui
if [[ -d "${ROOT}/third_party/imgui" ]]; then
  IMGUI="${ROOT}/third_party/imgui"
elif [[ -d "${ROOT}/third_party_imgui" ]]; then
  IMGUI="${ROOT}/third_party_imgui"
else
  echo "缺少 Dear ImGui：请放置到 third_party/imgui"
  exit 1
fi
CXX="${CXX:-clang++}"

mkdir -p "$OBJ" "$OUT"

if [[ ! -f "${NDKLIB}/libc++_static.a" ]]; then
  echo "缺少 ${NDKLIB}/libc++_static.a"
  exit 1
fi

INC=(
  -I"${ROOT}/include"
  -I"${ROOT}/src"
  -I"${ROOT}/third_party/lua-5.4.7/src"
  -I"${IMGUI}"
  -I"${IMGUI}/backends"
  -I"${PREFIX}/include"
)
DEFS=(-DVK_USE_PLATFORM_ANDROID_KHR -DANDROID)
FLAGS=(
  -std=c++20 -O2 -fPIC -fexceptions -Wall -Wno-unused-parameter
  -Wno-unknown-warning-option
  "${DEFS[@]}" "${INC[@]}"
)
SYS_LIBS=(
  "${SYS_LIB}/libvulkan.so"
  "${SYS_LIB}/libandroid.so"
  "${SYS_LIB}/liblog.so"
)

compile() {
  local src="$1"
  local base
  base="$(basename "$src" .cpp)"
  echo "  cxx ${base}"
  "$CXX" "${FLAGS[@]}" -c "$src" -o "${OBJ}/${base}.o"
}

CC="${CC:-clang}"
compile_c() {
  local src="$1"
  local base
  base="$(basename "$src" .c)"
  echo "  cc  ${base}"
  "$CC" -std=c11 -O2 -fPIC -Wall -Wno-unused-parameter \
    -DLUA_USE_POSIX \
    -I"${ROOT}/third_party/lua-5.4.7/src" \
    -c "$src" -o "${OBJ}/${base}.o"
}

echo "== embed icon_dump.dex =="
DEX_SRC="${OUT}/icon_dump.dex"
EMBED_H="${ROOT}/src/icon_dump_dex_embed.h"
if [[ -f "$DEX_SRC" ]]; then
  python3 - "$DEX_SRC" "$EMBED_H" <<'PY'
import sys, pathlib
src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
data = src.read_bytes()
with dst.open("w") as f:
    f.write("#pragma once\n// auto-generated from icon_dump.dex — do not edit\n")
    f.write("static const unsigned char k_icon_dump_dex[] = {\n")
    for i, b in enumerate(data):
        if i % 12 == 0: f.write("  ")
        f.write(f"0x{b:02x},")
        f.write("\n" if i % 12 == 11 else " ")
    if len(data) % 12: f.write("\n")
    f.write("};\n")
    f.write(f"static const unsigned int k_icon_dump_dex_len = {len(data)};\n")
print(f"  embedded {len(data)} bytes -> {dst}")
PY
else
  echo "warn: $DEX_SRC missing — keep existing embed header if any"
fi

# 若仅有 base64 字体，先还原 ttf
if [[ ! -f "${ROOT}/data/memdbg_cjk_subset.ttf" && -f "${ROOT}/data/memdbg_cjk_subset.ttf.b64" ]]; then
  echo "== restore font from base64 =="
  python3 - "${ROOT}/data/memdbg_cjk_subset.ttf.b64" "${ROOT}/data/memdbg_cjk_subset.ttf" <<'PYB'
import sys, base64, pathlib
src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
dst.write_bytes(base64.b64decode(src.read_text().encode()))
print(f"  restored {dst} ({dst.stat().st_size} bytes)")
PYB
fi

echo "== embed CJK font (zlib) =="
FONT_TTF="${ROOT}/data/memdbg_cjk_subset.ttf"
FONT_H="${ROOT}/src/font_embed.h"
if [[ -f "$FONT_TTF" ]]; then
  python3 - "$FONT_TTF" "$FONT_H" <<'PY'
import sys, pathlib, zlib
src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
raw = src.read_bytes()
z = zlib.compress(raw, 9)
with dst.open("w") as f:
    f.write("#pragma once\n// auto-generated CJK subset font (zlib) — do not edit\n")
    f.write(f"// raw_ttf={len(raw)} zlib={len(z)}\n")
    f.write(f"static const unsigned int k_embed_font_raw_len = {len(raw)};\n")
    f.write(f"static const unsigned int k_embed_font_z_len = {len(z)};\n")
    f.write("static const unsigned char k_embed_font_z[] = {\n")
    for i, b in enumerate(z):
        if i % 12 == 0: f.write("  ")
        f.write(f"0x{b:02x},")
        f.write("\n" if i % 12 == 11 else " ")
    if len(z) % 12: f.write("\n")
    f.write("};\n")
print(f"  embedded font zlib {len(z)} (raw {len(raw)}) -> {dst}")
PY
else
  if [[ ! -f "$FONT_H" ]]; then
    echo "error: missing $FONT_TTF and $FONT_H — cannot build without embedded font"
    exit 1
  fi
  echo "warn: $FONT_TTF missing — using existing $FONT_H"
fi

echo "== compile =="
# 清掉旧的系统 IME 目标，避免误链
rm -f "${OBJ}/sys_ime.o"

compile "${ROOT}/src/touch.cpp"
compile "${ROOT}/src/anwc_globals.cpp"
compile "${ROOT}/src/surface_create.cpp"
compile "${ROOT}/src/vk_engine.cpp"
compile "${ROOT}/src/mem_core.cpp"
compile "${ROOT}/src/mem_icon.cpp"
compile "${ROOT}/src/mem_disasm.cpp"
compile "${ROOT}/src/mem_sym.cpp"
compile "${ROOT}/src/mem_asm.cpp"
compile "${ROOT}/src/mem_game.cpp"
compile "${ROOT}/src/mem_lua.cpp"
compile "${ROOT}/src/mem_bp.cpp"

# ── 内嵌 Lua 5.4（静态，无外部 .so）────────────────────
LUA="${ROOT}/third_party/lua-5.4.7/src"
if [[ ! -f "${LUA}/lua.h" ]]; then
  echo "缺少 Lua 源码: ${LUA} （请下载 lua-5.4.7 到 third_party/）"
  exit 1
fi
echo "== lua 5.4 =="
for f in lapi lauxlib lbaselib lcode lcorolib lctype ldblib ldebug ldo ldump \
         lfunc lgc linit liolib llex lmathlib lmem loadlib lobject lopcodes \
         loslib lparser lstate lstring lstrlib ltable ltablib ltm lundump \
         lutf8lib lvm lzio; do
  compile_c "${LUA}/${f}.c"
done
compile "${ROOT}/src/mem_table.cpp"
compile "${ROOT}/src/mem_ptrscan.cpp"
compile "${ROOT}/src/mem_struct.cpp"
compile "${ROOT}/src/soft_ime.cpp"
compile "${ROOT}/src/float_app.cpp"
compile "${ROOT}/src/main.cpp"
compile "${IMGUI}/imgui.cpp"
compile "${IMGUI}/imgui_draw.cpp"
compile "${IMGUI}/imgui_tables.cpp"
compile "${IMGUI}/imgui_widgets.cpp"
compile "${IMGUI}/imgui_demo.cpp"
compile "${IMGUI}/backends/imgui_impl_vulkan.cpp"

echo "== link (single binary) =="
# 注意：不要链 Termux 默认 rpath（会优先找到错误的 libandroid stub）
# 只保留系统库路径，便于 /data/local/tmp 下直接执行
# 排除自测 / 工具 main（tools/*.cpp），避免 dual main
rm -f "${OBJ}/mem_selftest.o" "${OBJ}/test_"*.o "${OBJ}/quick_"*.o \
      "${OBJ}/probe_"*.o "${OBJ}/probe_dis.o" "${OBJ}/qq_feature_test.o" \
      "${OBJ}/maps_bug.o" "${OBJ}/test_hwbp"*.o
"$CXX" -std=c++20 -O2 -fPIC \
  "${OBJ}"/*.o \
  -nostdlib++ \
  -Wl,--push-state -Wl,--whole-archive \
  "${NDKLIB}/libc++_static.a" "${NDKLIB}/libc++abi.a" \
  -Wl,--pop-state \
  "${NDKLIB}/libunwind.a" \
  "${SYS_LIBS[@]}" \
  "${SYS_LIB}/libz.so" \
  -ldl -lm \
  -Wl,-rpath,/system/lib64 \
  -Wl,-rpath,/vendor/lib64 \
  -Wl,--enable-new-dtags \
  -Wl,--as-needed \
  -o "${OUT}/vk_imgui_float"

# 剥离 Termux 注入的 RUNPATH，只保留系统 so 搜索路径
echo "== fix rpath (system only) =="
python3 "${ROOT}/tools/fix_rpath.py" "${OUT}/vk_imgui_float" || {
  echo "warn: fix_rpath failed — 仍可尝试运行"
}

if command -v llvm-strip >/dev/null 2>&1; then
  llvm-strip --strip-unneeded "${OUT}/vk_imgui_float" 2>/dev/null || true
elif command -v strip >/dev/null 2>&1; then
  strip --strip-unneeded "${OUT}/vk_imgui_float" 2>/dev/null || true
fi
chmod 755 "${OUT}/vk_imgui_float"

echo "== verify =="
readelf -d "${OUT}/vk_imgui_float" 2>/dev/null | grep -E 'NEEDED|RPATH|RUNPATH' || true
ls -lh "${OUT}/vk_imgui_float"
file "${OUT}/vk_imgui_float"
echo ""
# 可选：构建 PackageManager 图标导出 dex（进程图标更完整）
if [[ -f "${ROOT}/tools/r8.jar" && -f "${ROOT}/java_ime/IconDump.java" ]]; then
  echo "== icon_dump.dex (optional) =="
  bash "${ROOT}/java_ime/build_icon_dump.sh" 2>/dev/null || \
    echo "warn: icon_dump.dex 构建失败（进程图标将仅用 APK 兜底）"
fi

echo "单文件产物: ${OUT}/vk_imgui_float"
echo "直接运行: su -c ${OUT}/vk_imgui_float"
echo "或: su -c 'sh ${ROOT}/run.sh'"
echo "图标 dex: ${OUT}/icon_dump.dex （刷新进程列表时自动调用）"
