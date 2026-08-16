#!/usr/bin/env python3
import hashlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SYS = ROOT / "vendor/elgato-hd60pro-1.1.0.194/extracted/e60MZ0380.X64.SYS"
OUT = ROOT / "vendor/elgato-hd60pro-1.1.0.194/logo-payloads"

DATA_FILE_OFF = 0x2EDC00
DATA_VA = 0x1402EF000
PAYLOAD_SIZE = 0x25800

PAYLOADS = [
    ("logo-selector-100.bin", 0x140319040, "Windows selector 0x100 copy source"),
    ("logo-selector-200.bin", 0x1402F3840, "Windows selector 0x200 copy source"),
    ("logo-selector-300.bin", 0x140319040, "Windows selector 0x300 observed copy source"),
    ("logo-source-14033e5c0.bin", 0x14033E5C0, "conversion/input source also referenced by functions 1 and 3"),
    ("logo-source-140318dc0.bin", 0x140318DC0, "conversion/input source referenced by function 2"),
]


def va_to_file_offset(va: int) -> int:
    if va < DATA_VA:
        raise ValueError(f"VA below .data: 0x{va:x}")
    return DATA_FILE_OFF + va - DATA_VA


def main() -> None:
    if not SYS.exists():
        raise SystemExit(f"missing Windows driver: {SYS}")

    data = SYS.read_bytes()
    OUT.mkdir(parents=True, exist_ok=True)

    for name, va, note in PAYLOADS:
        off = va_to_file_offset(va)
        blob = data[off:off + PAYLOAD_SIZE]
        if len(blob) != PAYLOAD_SIZE:
            raise SystemExit(f"{name}: short read at file offset 0x{off:x}")

        out = OUT / name
        out.write_bytes(blob)
        digest = hashlib.sha256(blob).hexdigest()
        print(f"{out} size={len(blob)} va=0x{va:x} file_off=0x{off:x} sha256={digest} note={note}")


if __name__ == "__main__":
    main()
