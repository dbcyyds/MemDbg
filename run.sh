#!/system/bin/sh
# 单文件：拷到 /data/local/tmp 后直接跑（RUNPATH 已指向系统 lib）
DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="$DIR/out/vk_imgui_float"
TMP="/data/local/tmp/vk_imgui_float"

if [ ! -x "$BIN" ]; then
  echo "missing $BIN — run: bash build.sh"
  exit 1
fi

cp -f "$BIN" "$TMP" && chmod 755 "$TMP"
exec "$TMP" "$@"
