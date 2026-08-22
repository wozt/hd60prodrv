#!/bin/sh
set -eu

ZIP="${1:-/home/wozt/Téléchargements/DVP Linux SDK v1.57.0(MZ0380).zip}"
LIB_IN_ZIP="DVP Linux SDK v1.57.0(MZ0380)/lib/libqcap.x64.so"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

if [ ! -r "$ZIP" ]; then
	echo "SDK zip not found: $ZIP" >&2
	exit 1
fi

unzip -p "$ZIP" "$LIB_IN_ZIP" >"$TMPDIR/libqcap.x64.so"

echo "== library =="
file "$TMPDIR/libqcap.x64.so"

echo
echo "== V4L2/QCAP symbols =="
nm -D --defined-only --demangle "$TMPDIR/libqcap.x64.so" |
	grep -E '^(0|[0-9a-f])+ [A-Z] (QCAP_|V4L2_GENERAL2::|__v4l2_general2__)' |
	sed -n '1,240p'

echo
echo "== V4L2 ioctl constants seen near V4L2_GENERAL2 =="
objdump -d --demangle \
	--start-address=0x386750 --stop-address=0x388040 \
	"$TMPDIR/libqcap.x64.so" |
	grep -E 'mov[[:space:]].*0x[0-9a-f]+,%esi|<ioctl@plt>' |
	sed -n '1,220p'

echo
echo "== strings of interest =="
strings -a "$TMPDIR/libqcap.x64.so" |
	grep -E '/dev/|VIDIOC|v4l2|V4L2|QCAP|YUY|YV12|UYV|NV12|MJPG|H264|audio|alsa|hw:' |
	sed -n '1,220p'
