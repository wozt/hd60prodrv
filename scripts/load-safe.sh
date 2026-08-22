#!/bin/sh
set -eu

MODULE="${1:-./hd60prodrv.ko}"

if [ "$#" -gt 0 ]; then
	shift
fi

if [ ! -r "$MODULE" ]; then
	echo "module not found: $MODULE" >&2
	echo "run 'make' from /home/wozt/hd60prodrv first" >&2
	exit 1
fi

SCRIPT_DIR="$(dirname -- "$0")"
if [ -x "$SCRIPT_DIR/pci-preflight-root.sh" ]; then
	"$SCRIPT_DIR/pci-preflight-root.sh" "${BDF:-0000:22:00.0}"
fi

if command -v modinfo >/dev/null 2>&1; then
	VERMAGIC="$(modinfo -F vermagic "$MODULE" 2>/dev/null | awk '{print $1}')"
	RUNNING="$(uname -r)"
	if [ -n "$VERMAGIC" ] && [ "$VERMAGIC" != "$RUNNING" ]; then
		echo "refusing to load $MODULE: vermagic is $VERMAGIC but running kernel is $RUNNING" >&2
		exit 1
	fi
else
	echo "warning: modinfo not available; cannot verify module vermagic before insmod" >&2
fi

SIGN_KEY="$(dirname -- "$0")/../certs/MOK.priv"
SIGN_CERT="$(dirname -- "$0")/../certs/MOK.pem"
SIGN_FILE="$(find /usr/lib -name sign-file 2>/dev/null | sort -V | tail -1)"
if [ -f "$SIGN_KEY" ] && [ -f "$SIGN_CERT" ] && [ -n "$SIGN_FILE" ]; then
	"$SIGN_FILE" sha256 "$SIGN_KEY" "$SIGN_CERT" "$MODULE"
fi

if command -v modprobe >/dev/null 2>&1; then
	sudo modprobe videodev
	sudo modprobe videobuf2-common
	sudo modprobe videobuf2-v4l2
	sudo modprobe videobuf2-vmalloc
fi

sudo insmod "$MODULE" "$@"
dmesg | grep hd60prodrv | tail -20
