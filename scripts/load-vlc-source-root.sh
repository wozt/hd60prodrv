#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root" >&2
	exit 1
fi

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$ROOT"

rmmod hd60prodrv 2>/dev/null || true

./scripts/load-safe.sh ./hd60prodrv.ko \
	mmio_dump=1 \
	enable_v4l2=1 \
	synthetic_v4l2=1 \
	mailbox_bar=0

echo
echo "V4L2 devices:"
v4l2-ctl --list-devices 2>/dev/null || true
echo
echo "Open with VLC, adjusting the node if another camera owns /dev/video0:"
echo "  vlc v4l2:///dev/video0"
