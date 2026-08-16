#!/bin/sh
set -eu

SRC="${1:-vendor/elgato-hd60pro-1.1.0.194/extracted}"
DST="${2:-/lib/firmware/hd60prodrv}"

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo $0 [source-dir] [destination-dir]" >&2
	exit 1
fi

if [ ! -d "$SRC" ]; then
	echo "source directory not found: $SRC" >&2
	echo "run ./scripts/extract-elgato-driver.sh first" >&2
	exit 1
fi

mkdir -p "$DST"
for fw in MZ0380.HD.HEX MZ0380.SD.HEX MZ0381.HD.HEX MZ0381.SD.HEX; do
	if [ -r "$SRC/$fw" ]; then
		install -m 0644 "$SRC/$fw" "$DST/$fw"
		echo "installed $DST/$fw"
	fi
done
