#!/bin/sh
set -eu

BDF="${1:-0000:22:00.0}"
MODULE="${2:-./hd60prodrv.ko}"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="snapshots/${STAMP}-${BDF}-loaded"

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo $0 [BDF] [module]" >&2
	exit 1
fi

if [ ! -r "$MODULE" ]; then
	echo "module not found: $MODULE" >&2
	exit 1
fi

if ! mountpoint -q /sys/kernel/debug; then
	mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
fi

if lsmod | grep -q '^hd60prodrv\b'; then
	rmmod hd60prodrv
fi

modprobe videodev
insmod "$MODULE" mmio_dump=1 enable_v4l2=1
trap 'rmmod hd60prodrv 2>/dev/null || true' EXIT
./scripts/snapshot.sh "$BDF" "$OUT"

if command -v v4l2-ctl >/dev/null 2>&1; then
	v4l2-ctl --list-devices >"$OUT/v4l2-list-devices.txt" 2>&1 || true
	for dev in /dev/video*; do
		[ -e "$dev" ] || continue
		v4l2-ctl -d "$dev" --all >"$OUT/v4l2-all-$(basename "$dev").txt" 2>&1 || true
		v4l2-ctl -d "$dev" --list-formats-ext >"$OUT/v4l2-formats-$(basename "$dev").txt" 2>&1 || true
	done
fi

dmesg | grep hd60prodrv >"$OUT/dmesg-after-snapshot.txt" 2>&1 || true
rmmod hd60prodrv
trap - EXIT
dmesg | grep hd60prodrv >"$OUT/dmesg-after-unload.txt" 2>&1 || true

echo "$OUT"
