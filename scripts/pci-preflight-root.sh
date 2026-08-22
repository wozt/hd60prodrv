#!/bin/sh
set -eu

BDF="${1:-${BDF:-0000:22:00.0}}"
DEV="/sys/bus/pci/devices/$BDF"
CONFIG="$DEV/config"

if [ ! -d "$DEV" ]; then
	echo "$BDF is not present under /sys/bus/pci/devices" >&2
	exit 20
fi

if [ ! -r "$CONFIG" ]; then
	echo "$BDF PCI config space is not readable" >&2
	exit 21
fi

CONFIG16="$(od -An -tx1 -N 16 "$CONFIG" 2>/dev/null | tr -d ' \n')"
HEADER="$(od -An -tx1 -j 14 -N 1 "$CONFIG" 2>/dev/null | tr -d ' \n')"

case "$CONFIG16" in
	ffffffffffffffffffffffffffffffff)
		echo "$BDF PCI config reads all 0xff; the card is wedged/inaccessible." >&2
		echo "Do a full PSU cold boot before running HD60 Pro mailbox, firmware, V4L2, or VLC tests." >&2
		exit 22
		;;
esac

if [ "$HEADER" = "7f" ] || [ "$HEADER" = "ff" ]; then
	echo "$BDF PCI header type is 0x$HEADER; the card is not in a usable PCI state." >&2
	echo "Do a full PSU cold boot before running HD60 Pro mailbox, firmware, V4L2, or VLC tests." >&2
	exit 23
fi

VENDOR="$(cat "$DEV/vendor" 2>/dev/null || true)"
DEVICE="$(cat "$DEV/device" 2>/dev/null || true)"
if [ "$VENDOR" != "0x12ab" ] || [ "$DEVICE" != "0x0380" ]; then
	echo "$BDF is $VENDOR:$DEVICE, expected 0x12ab:0x0380" >&2
	exit 24
fi

echo "$BDF PCI preflight OK: vendor=$VENDOR device=$DEVICE header=0x$HEADER"
