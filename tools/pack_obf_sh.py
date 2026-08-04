#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MemDbg → UPX → gzip → XOR → 类 /data/local/dbc21_.sh 单文件

格式（与 dbc21_.sh 相同结构）:
  第 1 行: 引导 shell（root + busybox + 快速解密 + 执行）
  第 2 行起: 加密二进制乱码

解密使用内嵌的小型动态链接 mdbg_dec（数 KB），避免 awk 扫 1MB 过慢，
也避免 printf 大参数触发 ARG_MAX。

用法:
  python3 tools/pack_obf_sh.py
  python3 tools/pack_obf_sh.py --install /data/local/dbc21_.sh
"""

from __future__ import annotations

import argparse
import base64
import gzip
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = Path(__file__).resolve().parent
DEFAULT_BIN = ROOT / "out" / "vk_imgui_float"
DEFAULT_OUT = ROOT / "out" / "dbc21_.sh"
DEFAULT_INSTALL = "/data/local/dbc21_.sh"
DEFAULT_PK = "DBCABADB5F5EC5D8472074BEB885411C"
DEC_SRC = TOOLS / "mdbg_dec.c"
DEC_BIN = TOOLS / "mdbg_dec_dyn"


def ensure_decoder() -> bytes:
    """编译（如需要）并返回动态链接解密器字节。"""
    need = True
    if DEC_BIN.is_file() and DEC_SRC.is_file():
        if DEC_BIN.stat().st_mtime >= DEC_SRC.stat().st_mtime:
            need = False
    if need:
        cc = shutil.which("clang") or shutil.which("gcc")
        if not cc:
            raise SystemExit("需要 clang/gcc 编译 mdbg_dec")
        print(f"[*] 编译解密器: {cc} -O2 -s")
        r = subprocess.run(
            [cc, "-O2", "-s", "-o", str(DEC_BIN), str(DEC_SRC)],
            capture_output=True,
            text=True,
        )
        if r.returncode != 0:
            print(r.stderr, file=sys.stderr)
            raise SystemExit("编译 mdbg_dec 失败")
    data = DEC_BIN.read_bytes()
    print(f"[+] 解密器: {DEC_BIN} ({len(data)} bytes)")
    return data


def run_upx(src: Path, dst: Path) -> Path:
    upx = shutil.which("upx")
    if not upx:
        print("[!] 无 upx，使用原文件", file=sys.stderr)
        shutil.copy2(src, dst)
        return dst
    if dst.exists():
        dst.unlink()
    r = subprocess.run(
        [upx, "-9", "-o", str(dst), str(src)], capture_output=True, text=True
    )
    if r.returncode != 0:
        err = r.stderr or r.stdout or ""
        if "already packed" in err.lower() or "AlreadyPacked" in err:
            shutil.copy2(src, dst)
            return dst
        r2 = subprocess.run(
            [upx, "-9", "--force", "-o", str(dst), str(src)],
            capture_output=True,
            text=True,
        )
        if r2.returncode != 0:
            print(err, file=sys.stderr)
            shutil.copy2(src, dst)
            return dst
    print(f"[+] UPX: {src.stat().st_size} -> {dst.stat().st_size}")
    return dst


def derive_keys_hex(pk: str) -> tuple[str, str, str]:
    def hcut(tag: str, nhex: int) -> str:
        return hashlib.sha256(f"DBC|{tag}|{pk}".encode()).hexdigest()[:nhex]

    return hcut("A", 32), hcut("B", 48), hcut("C", 28)


def xor_crypt(data: bytes, ka_hex: str, kb_hex: str, kc_hex: str) -> bytes:
    ka, kb, kc = bytes.fromhex(ka_hex), bytes.fromhex(kb_hex), bytes.fromhex(kc_hex)
    out = bytearray(len(data))
    la, lb, lc = len(ka), len(kb), len(kc)
    for i, b in enumerate(data):
        x = b
        x ^= (i & 255) ^ ((i // 256) & 255) ^ kc[i % lc]
        x ^= kb[i % lb]
        x ^= ka[i % la]
        out[i] = x
    return bytes(out)


def build_stub(pk: str, tag: str, payload_size: int, dec_b64: str) -> str:
    """
    单行引导:
      root → busybox → 缓存命中则 exec
      → tail 剥离载荷 → 释放 mdbg_dec → 解密 → gzip -cd → 缓存 → exec
    """
    # base64 按 76 列无换行塞进单行变量（用短变量名）
    stub = (
        'if [ `id -u` -ne 0 ];then printf "\\350\\257\\267\\344\\273\\245ROOT\\346\\235\\203\\351\\231\\220\\346\\211\\247\\350\\241\\214\\357\\274\\201\\n";exit 1;fi;'
        'export PATH="/data/adb/magisk:/data/adb/ksu/bin:/data/adb/ap/bin:/system/xbin:/system/bin:/sbin:/data/data/com.termux/files/usr/bin:$PATH";'
        '_BB="";'
        "for b in /data/adb/magisk/busybox /data/adb/ksu/bin/busybox /data/adb/ap/bin/busybox busybox;"
        'do [ -x "$b" ]&&_BB=$b&&break;done;'
        '[ -n "$_BB" ]||{ printf "busybox?\\n";exit 1;};'
        '_CARD=${0##*/};_CARD=${_CARD%.sh};_CARD=${_CARD%.SH};export DBC_USER_KEY="$_CARD";'
        f'_PK="{pk}";_TAG="{tag}";_ESZ={payload_size};'
        '_CACHE="/data/local/tmp/.m_${_TAG}";'
        'if [ -x "$_CACHE" ];then _cs=`wc -c <"$_CACHE" 2>/dev/null||echo 0`;'
        '[ "$_cs" -gt 100000 ]&&exec "$_CACHE" "$@";fi;'
        '_sha(){ _o=`printf %s "$1"|"$_BB" sha256sum 2>/dev/null`;'
        '[ -n "$_o" ]||_o=`printf %s "$1"|sha256sum 2>/dev/null`;'
        "echo \"$_o\"|awk '{print $1}';};"
        '_KA=`_sha "DBC|A|$_PK"|cut -c1-32`;'
        '_KB=`_sha "DBC|B|$_PK"|cut -c1-48`;'
        '_KC=`_sha "DBC|C|$_PK"|cut -c1-28`;'
        'a=/data/local/tmp/;b=`"$_BB" mktemp -d "${a}.xXXXXXX" 2>/dev/null`||b="${a}.x$$";'
        'mkdir -p "$b";_um=`umask`;umask 077;c="$b/i";'
        # 二进制安全：只跳过首行
        'case `printf "X\\n"|tail -n+1 2>/dev/null` in X)_td=-n;;*)_td=;esac;'
        'tail $_td+2<"$0">"$c.e"||{ rm -fr "$b";exit 1;};'
        '_ez=`wc -c <"$c.e" 2>/dev/null||echo 0`;'
        '[ "$_ez" -eq "$_ESZ" ]||{ printf "payload size mismatch %s!=%s\\n" "$_ez" "$_ESZ";rm -fr "$b";exit 1;};'
        # 释放解密器（小，printf 安全）
        f'_DB64="{dec_b64}";'
        'printf %s "$_DB64"|"$_BB" base64 -d>"$c.dec" 2>/dev/null||printf %s "$_DB64"|base64 -d>"$c.dec";'
        'chmod 700 "$c.dec";'
        # 快速 XOR 解密 → gzip 流
        '"$c.dec" "$c.e" "$_KA" "$_KB" "$_KC">"$c.gz"||{ printf "decrypt fail\\n";rm -fr "$b";exit 1;};'
        'if "$_BB" gzip -cd "$c.gz">"$c" 2>/dev/null;then '
        'umask $_um;chmod 700 "$c";'
        # $c 位于临时目录 $b，必须先拷到缓存再删 $b，exec 缓存路径
        'cp -f "$c" "$_CACHE";chmod 700 "$_CACHE";rm -rf "$b";'
        'exec "$_CACHE" "$@";'
        'else printf "gzip fail\\n";rm -fr "$b";exit 1;fi;exit'
    )
    return stub + "\n"


def pack(
    bin_path: Path,
    out_path: Path,
    pk: str,
    do_upx: bool,
    keep_upx: Path | None,
    install: str | None,
) -> Path:
    if not bin_path.is_file():
        raise SystemExit(f"二进制不存在: {bin_path}")

    dec = ensure_decoder()
    dec_b64 = base64.b64encode(dec).decode("ascii")

    work = Path(tempfile.mkdtemp(prefix="mdbg_pack_"))
    try:
        upx_path = work / "bin.upx"
        if do_upx:
            run_upx(bin_path, upx_path)
        else:
            shutil.copy2(bin_path, upx_path)
        if keep_upx:
            keep_upx.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(upx_path, keep_upx)
            print(f"[+] UPX 副本: {keep_upx} ({keep_upx.stat().st_size})")

        raw = upx_path.read_bytes()
        gz = gzip.compress(raw, compresslevel=9)
        print(f"[+] gzip: {len(raw)} -> {len(gz)}")

        ka, kb, kc = derive_keys_hex(pk)
        enc = xor_crypt(gz, ka, kb, kc)
        # 用真实解密器闭环验证
        t_enc = work / "t.enc"
        t_enc.write_bytes(enc)
        r = subprocess.run(
            [str(DEC_BIN), str(t_enc), ka, kb, kc], capture_output=True
        )
        if r.returncode != 0 or gzip.decompress(r.stdout) != raw:
            raise SystemExit("解密器闭环验证失败")
        print(f"[+] XOR 加密: {len(enc)} bytes (解密器验证 OK)")

        tag = hashlib.md5(raw[:4096] + pk.encode()).hexdigest()[:12]
        stub = build_stub(pk, tag, len(enc), dec_b64)
        if stub.count("\n") != 1:
            raise SystemExit("stub 必须单行")

        out_path.parent.mkdir(parents=True, exist_ok=True)
        with open(out_path, "wb") as f:
            f.write(stub.encode("utf-8"))
            f.write(enc)
        os.chmod(out_path, 0o755)
        print(f"[+] 写出: {out_path} ({out_path.stat().st_size} bytes)")
        print(f"    引导: {len(stub)} | 载荷: {len(enc)} | 解密器b64: {len(dec_b64)}")
        print(f"    运行: su -c 'sh {out_path} --help'")

        if install:
            cmd = (
                f"cp -f '{out_path}' '{install}' && chmod 755 '{install}' && "
                f"rm -f /data/local/tmp/.m_{tag} && ls -la '{install}'"
            )
            rr = subprocess.run(["su", "-c", cmd], capture_output=True, text=True)
            if rr.returncode != 0:
                print(f"[!] 安装失败: {rr.stderr or rr.stdout}", file=sys.stderr)
            else:
                print(f"[+] 已安装: {install}")
                print(rr.stdout.strip())
        return out_path
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main() -> None:
    ap = argparse.ArgumentParser(description="UPX + dbc21_ 风格加密 sh")
    ap.add_argument("-i", "--input", type=Path, default=DEFAULT_BIN)
    ap.add_argument("-o", "--output", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--pk", default=DEFAULT_PK)
    ap.add_argument("--no-upx", action="store_true")
    ap.add_argument("--upx-out", type=Path, default=ROOT / "out" / "vk_imgui_float.upx")
    ap.add_argument("--install", default=DEFAULT_INSTALL)
    ap.add_argument("--no-install", action="store_true")
    args = ap.parse_args()
    pack(
        bin_path=args.input,
        out_path=args.output,
        pk=args.pk,
        do_upx=not args.no_upx,
        keep_upx=args.upx_out,
        install=None if args.no_install else args.install,
    )


if __name__ == "__main__":
    main()
