#!/usr/bin/env bash
# IconDump.java → out/icon_dump.dex
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JIME="$ROOT/java_ime"
R8="$ROOT/tools/r8.jar"
OUT="$ROOT/out"

[[ -f "$R8" ]] || { echo "缺少 $R8"; exit 1; }

rm -rf "$JIME/icon_classes" "$JIME/icon_dex"
mkdir -p "$JIME/icon_classes" "$JIME/icon_dex" "$OUT"

echo "== javac IconDump =="
javac -source 11 -target 11 -encoding UTF-8 -Xlint:unchecked \
  -d "$JIME/icon_classes" "$JIME/IconDump.java" 2>&1 | grep -v unchecked || true

# 确保 class 生成
[[ -f "$JIME/icon_classes/IconDump.class" ]] || \
  javac -source 11 -target 11 -encoding UTF-8 -d "$JIME/icon_classes" "$JIME/IconDump.java"

echo "== d8 =="
java -cp "$R8" com.android.tools.r8.D8 \
  --release --min-api 26 \
  --output "$JIME/icon_dex" \
  $(find "$JIME/icon_classes" -name '*.class')

cp -f "$JIME/icon_dex/classes.dex" "$OUT/icon_dump.dex"
ls -lh "$OUT/icon_dump.dex"
echo "OK: $OUT/icon_dump.dex"
echo "Test: su -c 'cp $OUT/icon_dump.dex /data/local/tmp/ && CLASSPATH=/data/local/tmp/icon_dump.dex app_process64 /system/bin IconDump /data/local/tmp/memdbg_icons com.tencent.mm'"
