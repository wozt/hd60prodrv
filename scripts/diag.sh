#!/bin/sh
set -eu

BDF="${1:-0000:22:00.0}"
DEV="/sys/bus/pci/devices/$BDF"

echo "== system =="
uname -a

echo
echo "== pci =="
lspci -nn -s "$BDF" -vvv || true

echo
echo "== sysfs ids =="
for f in vendor device subsystem_vendor subsystem_device class irq numa_node resource; do
	if [ -r "$DEV/$f" ]; then
		echo "-- $f --"
		cat "$DEV/$f"
	fi
done

echo
echo "== pci config first 256 bytes =="
if command -v xxd >/dev/null 2>&1; then
	xxd -g1 -l 256 "$DEV/config" || true
else
	od -Ax -tx1 -N 256 "$DEV/config" || true
fi

echo
echo "== video devices =="
find /dev -maxdepth 1 \( -name 'video*' -o -name 'v4l*' \) -print 2>/dev/null || true

echo
echo "== module state =="
lsmod | grep -E '^hd60prodrv\b' || true
