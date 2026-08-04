#!/usr/bin/env bash
# 编译 SysIme.java → out/sys_ime.dex（纯反射，无需 android.jar）
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JIME="$ROOT/java_ime"
R8="$ROOT/tools/r8.jar"
OUT="$ROOT/out"

[[ -f "$R8" ]] || { echo "缺少 $R8"; exit 1; }

rm -rf "$JIME/classes" "$JIME/dex_out"
mkdir -p "$JIME/classes" "$JIME/dex_out" "$OUT"

echo "== javac SysIme =="
javac -source 11 -target 11 -encoding UTF-8 -d "$JIME/classes" "$JIME/SysIme.java"

echo "== d8 =="
java -cp "$R8" com.android.tools.r8.D8 \
  --release --min-api 26 \
  --output "$JIME/dex_out" \
  $(find "$JIME/classes" -name '*.class')

cp -f "$JIME/dex_out/classes.dex" "$OUT/sys_ime.dex"
ls -lh "$OUT/sys_ime.dex"
echo "OK: $OUT/sys_ime.dex"
echo "Test: su -c 'cp $OUT/sys_ime.dex /data/local/tmp/ && CLASSPATH=/data/local/tmp/sys_ime.dex app_process64 /system/bin SysIme /data/local/tmp/t 标题 初值 text'"
