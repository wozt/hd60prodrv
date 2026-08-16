#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root" >&2
	exit 1
fi

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SRC="$ROOT/vendor/elgato-hd60pro-1.1.0.194/logo-payloads"
DST="/lib/firmware/hd60prodrv"

if [ ! -f "$SRC/logo-selector-100.bin" ] || \
   [ ! -f "$SRC/logo-selector-200.bin" ] || \
   [ ! -f "$SRC/logo-selector-300.bin" ]; then
	echo "payloads missing; run scripts/extract-logo-payloads.py first" >&2
	exit 1
fi

mkdir -p "$DST"
install -m 0644 "$SRC/logo-selector-100.bin" "$DST/logo-selector-100.bin"
install -m 0644 "$SRC/logo-selector-200.bin" "$DST/logo-selector-200.bin"
install -m 0644 "$SRC/logo-selector-300.bin" "$DST/logo-selector-300.bin"

echo "installed $DST/logo-selector-100.bin"
echo "installed $DST/logo-selector-200.bin"
echo "installed $DST/logo-selector-300.bin"
