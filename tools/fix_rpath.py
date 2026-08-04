#!/usr/bin/env python3
"""Set ELF DT_RUNPATH/RPATH to system lib dirs only (strip Termux paths)."""
import struct
import sys
from pathlib import Path

# New runpath — must fit in existing string table slot or we only shrink-in-place
NEW = b"/system/lib64:/vendor/lib64:/system_ext/lib64"


def patch(path: Path) -> None:
    data = bytearray(path.read_bytes())
    # Find common Termux/system runpath fragments and rewrite
    # Prefer exact known pattern first
    candidates = []
    # Scan for "/system/lib64" occurrences and nearby runpath strings
    needle = b"/data/data/com.termux"
    i = 0
    while True:
        j = data.find(needle, i)
        if j < 0:
            break
        # walk back to start of string (previous NUL)
        start = j
        while start > 0 and data[start - 1] != 0:
            start -= 1
        end = j
        while end < len(data) and data[end] != 0:
            end += 1
        s = bytes(data[start:end])
        if b"lib" in s and (b"rpath" in s.lower() or b"system/lib" in s or b"termux" in s or b":" in s):
            candidates.append((start, end, s))
        # also any string that contains both termux and system/lib64
        if b"termux" in s and b"system/lib" in s:
            candidates.append((start, end, s))
        i = j + 1

    # Also find pure long RUNPATH with system/lib64 and termux
    i = 0
    while True:
        j = data.find(b"/system/lib64", i)
        if j < 0:
            break
        start = j
        while start > 0 and data[start - 1] != 0:
            start -= 1
        end = j
        while end < len(data) and data[end] != 0:
            end += 1
        s = bytes(data[start:end])
        if b":" in s or b"termux" in s:
            candidates.append((start, end, s))
        i = j + 1

    # unique by start
    seen = set()
    uniq = []
    for c in candidates:
        if c[0] not in seen:
            seen.add(c[0])
            uniq.append(c)

    if not uniq:
        # try inject by finding empty or short path — fallback: search RUNPATH tag via dynamic section
        print("warn: no termux/system runpath string found; trying DT scan")
        # Minimal ELF64 parse for DT_RUNPATH string
        if data[:4] != b"\x7fELF":
            raise SystemExit("not ELF")
        ei_class = data[4]
        if ei_class != 2:
            raise SystemExit("only ELF64")
        e_phoff = struct.unpack_from("<Q", data, 32)[0]
        e_phentsize = struct.unpack_from("<H", data, 54)[0]
        e_phnum = struct.unpack_from("<H", data, 56)[0]
        dyn_off = dyn_sz = 0
        for i in range(e_phnum):
            off = e_phoff + i * e_phentsize
            p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from(
                "<IIQQQQQQ", data, off
            )
            if p_type == 2:  # PT_DYNAMIC
                dyn_off, dyn_sz = p_offset, p_filesz
                break
        if not dyn_off:
            raise SystemExit("no PT_DYNAMIC")
        strtab = None
        runpath_off = None
        rpath_off = None
        pos = dyn_off
        while pos < dyn_off + dyn_sz:
            d_tag, d_val = struct.unpack_from("<Qq", data, pos)
            pos += 16
            if d_tag == 0:
                break
            if d_tag == 5:  # DT_STRTAB
                strtab_vaddr = d_val
            if d_tag == 0x1D:  # DT_RUNPATH
                runpath_off = d_val
            if d_tag == 15:  # DT_RPATH
                rpath_off = d_val
        # map vaddr to file via PT_LOAD
        def v2f(v):
            for i in range(e_phnum):
                off = e_phoff + i * e_phentsize
                p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from(
                    "<IIQQQQQQ", data, off
                )
                if p_type == 1 and p_vaddr <= v < p_vaddr + p_filesz:
                    return p_offset + (v - p_vaddr)
            return None

        # re-parse for STRTAB
        pos = dyn_off
        strtab_vaddr = None
        while pos < dyn_off + dyn_sz:
            d_tag, d_val = struct.unpack_from("<Qq", data, pos)
            pos += 16
            if d_tag == 0:
                break
            if d_tag == 5:
                strtab_vaddr = d_val
            if d_tag == 0x1D:
                runpath_off = d_val
            if d_tag == 15:
                rpath_off = d_val
        if strtab_vaddr is None:
            raise SystemExit("no STRTAB")
        strtab_file = v2f(strtab_vaddr)
        for name, off in (("RUNPATH", runpath_off), ("RPATH", rpath_off)):
            if off is None:
                continue
            start = strtab_file + off
            end = start
            while end < len(data) and data[end] != 0:
                end += 1
            old = bytes(data[start:end])
            print(f"found DT_{name}: {old!r} len={end-start}")
            if len(NEW) > end - start:
                # truncate new to fit
                repl = NEW[: end - start]
            else:
                repl = NEW
            data[start:end] = repl + b"\x00" * (end - start - len(repl))
            print(f"patched DT_{name} -> {repl!r}")
        path.write_bytes(data)
        print("OK", path)
        return

    patched = 0
    for start, end, s in uniq:
        slot = end - start
        print(f"slot@{start}: {s!r} ({slot} bytes)")
        if b"termux" not in s and b"usr/lib" not in s and s != b"/system/lib64":
            # only patch paths that include termux or multi-path
            if b":" not in s:
                continue
        repl = NEW if len(NEW) <= slot else NEW[:slot]
        data[start:end] = repl + b"\x00" * (slot - len(repl))
        print(f"  -> {repl!r}")
        patched += 1

    if not patched:
        raise SystemExit("nothing patched")
    path.write_bytes(data)
    print("OK patched", patched, "string(s) in", path)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <elf>")
        sys.exit(1)
    patch(Path(sys.argv[1]))
