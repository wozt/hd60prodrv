#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root" >&2
	exit 1
fi

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$ROOT"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

DEVICE="${DEVICE:-/dev/video0}"
FRAMES="${FRAMES:-4}"
OUT="${OUT:-/tmp/hd60prodrv-real-v4l2.yuyv}"
FRAME_BYTES="${FRAME_BYTES:-4147200}"

if ! command -v v4l2-ctl >/dev/null 2>&1; then
	echo "v4l2-ctl is required" >&2
	exit 1
fi

if [ "${LOAD_FIRST:-1}" != "0" ]; then
	./scripts/load-vlc-real-root.sh
fi

DBG="/sys/kernel/debug/hd60prodrv/0000:22:00.0"

echo "== capture_info before =="
if [ -r "$DBG/capture_info" ]; then
	cat "$DBG/capture_info"
else
	echo "capture_info unavailable"
fi

rm -f "$OUT"
echo
echo "== capture $FRAMES frame(s) from $DEVICE =="
v4l2-ctl -d "$DEVICE" --stream-mmap=4 --stream-count="$FRAMES" --stream-to="$OUT"

echo
echo "== analyze raw YUYV =="
set +e
python3 - "$OUT" "$FRAME_BYTES" <<'PY'
import os
import sys

path = sys.argv[1]
frame_bytes = int(sys.argv[2], 0)
data = open(path, "rb").read()

if frame_bytes <= 0:
    raise SystemExit("invalid FRAME_BYTES")
if len(data) < frame_bytes:
    raise SystemExit(f"capture too small: {len(data)} bytes, expected at least {frame_bytes}")

frames = len(data) // frame_bytes
print(f"file={path}")
print(f"bytes={len(data)} frame_bytes={frame_bytes} complete_frames={frames}")

fallback = bytes((16, 128, 16, 128))
real_candidate = False
for idx in range(frames):
    frame = data[idx * frame_bytes:(idx + 1) * frame_bytes]
    sample = frame[:min(len(frame), 1024 * 1024)]
    nonzero = sum(1 for b in sample if b)
    diff = 0
    for off, b in enumerate(sample):
        if b != fallback[off & 3]:
            diff += 1
    diff_pct = (diff * 100.0) / len(sample)
    nonzero_pct = (nonzero * 100.0) / len(sample)
    print(f"frame[{idx}]: sample_nonzero={nonzero}/{len(sample)} ({nonzero_pct:.4f}%) fallback_diff={diff_pct:.4f}%")
    if diff_pct > 0.5 and nonzero_pct > 1.0:
        real_candidate = True

if real_candidate:
    print("result: NON-FALLBACK frame data detected")
    raise SystemExit(0)

print("result: captured data is still fallback-like or empty")
raise SystemExit(2)
PY
ANALYZE_STATUS=$?
set -e

echo
echo "== capture_info after =="
if [ -r "$DBG/capture_info" ]; then
	cat "$DBG/capture_info" | tee "$TMPDIR/capture-after.txt"
	LAST_EXTRA="$(sed -n 's/^last_frame_extra: //p' "$TMPDIR/capture-after.txt" | tail -1)"
	DMA_FRAMES="$(sed -n 's/^dma_frame_count: //p' "$TMPDIR/capture-after.txt" | tail -1)"
	if [ "$LAST_EXTRA" = "0x00000000" ] && [ "${DMA_FRAMES:-0}" != "0" ] && [ "$ANALYZE_STATUS" -eq 0 ]; then
		echo
		echo "metadata_result: REAL_DMA_FRAME_VERIFIED"
		exit 0
	elif [ "$LAST_EXTRA" = "0x00000000" ] && [ "${DMA_FRAMES:-0}" != "0" ]; then
		echo
		echo "metadata_result: REAL_DMA_METADATA_ONLY"
		echo "note: DMA metadata advanced, but captured YUYV still looked fallback-like"
		exit 15
	fi
fi

exit "$ANALYZE_STATUS"
