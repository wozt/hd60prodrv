#!/usr/bin/env python3
import argparse
import bisect
import re
import subprocess
from pathlib import Path


SECTIONS = [
    (".text", 0x00000400, 0x002a2f37, 0x140001000),
    ("RT_CODE", 0x002a3400, 0x00000815, 0x1402a4000),
    (".rdata", 0x002a3e00, 0x00049ce8, 0x1402a5000),
    (".data", 0x002edc00, 0x000ace00, 0x1402ef000),
    (".pdata", 0x0039aa00, 0x0000ca5c, 0x1403ae000),
    ("INIT", 0x003a7600, 0x00000b4a, 0x1403bb000),
    (".rsrc", 0x003a8200, 0x00000350, 0x1403bc000),
    (".reloc", 0x003a8600, 0x000032a8, 0x1403bd000),
]


def fileoff_to_va(off):
    for name, start, size, va in SECTIONS:
        if start <= off < start + size:
            return name, va + off - start
    return None, None


def read_strings(path, encoding):
    cmd = ["strings", "-a", "-td"]
    if encoding == "utf16le":
        cmd.append("-el")
    cmd.append(str(path))
    out = subprocess.check_output(cmd, text=True, errors="replace")
    for line in out.splitlines():
        m = re.match(r"\s*(\d+)\s+(.*)", line)
        if not m:
            continue
        yield int(m.group(1)), m.group(2)


def load_functions(objdump_path):
    starts = []
    for line in objdump_path.read_text(errors="replace").splitlines():
        m = re.match(r"\s*([0-9a-f]+)\s+<[^>]+>:", line)
        if m:
            starts.append(int(m.group(1), 16))
    starts.sort()
    return starts


def nearest_func(starts, addr):
    if not starts:
        return None
    idx = bisect.bisect_right(starts, addr) - 1
    if idx < 0:
        return None
    return starts[idx]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sys")
    ap.add_argument("objdump")
    ap.add_argument("patterns", nargs="+")
    args = ap.parse_args()

    sys_path = Path(args.sys)
    objdump_path = Path(args.objdump)
    objdump = objdump_path.read_text(errors="replace").splitlines()
    funcs = load_functions(objdump_path)

    wanted = []
    for enc in ("ascii", "utf16le"):
        for off, text in read_strings(sys_path, enc):
            if any(p in text for p in args.patterns):
                sec, va = fileoff_to_va(off)
                wanted.append((text, enc, off, sec, va))

    for text, enc, off, sec, va in sorted(wanted, key=lambda x: (x[4] or 0, x[0])):
        print(f"\n{text}")
        print(f"  enc={enc} file_off=0x{off:x} section={sec} va={va:#x}" if va else
              f"  enc={enc} file_off=0x{off:x} section=? va=?")
        if not va:
            continue
        needle = f"# {va:#x}"
        hits = []
        for i, line in enumerate(objdump):
            if needle in line:
                m = re.match(r"\s*([0-9a-f]+):", line)
                insn = int(m.group(1), 16) if m else 0
                func = nearest_func(funcs, insn)
                hits.append((i + 1, insn, func, line.strip()))
        if not hits:
            print("  xrefs: none found in objdump comments")
            continue
        print("  xrefs:")
        for lineno, insn, func, line in hits[:20]:
            print(f"    line={lineno} insn={insn:#x} func={func:#x} {line}")
        if len(hits) > 20:
            print(f"    ... {len(hits) - 20} more")


if __name__ == "__main__":
    main()
