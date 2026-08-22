#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo $0" >&2
	exit 1
fi

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$ROOT"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

DEVICE="${DEVICE:-/dev/video0}"
FRAMES="${FRAMES:-4}"
OUT="${OUT:-/tmp/hd60prodrv-cold-real-v4l2.yuyv}"
FRAME_BYTES="${FRAME_BYTES:-4147200}"
PREINIT_TIMEOUT="${PREINIT_TIMEOUT:-65s}"
FIRMWARE_LOAD_TIMEOUT="${FIRMWARE_LOAD_TIMEOUT:-220s}"

if ! command -v v4l2-ctl >/dev/null 2>&1; then
	echo "v4l2-ctl is required" >&2
	exit 1
fi

echo "== prepare power, no PCI reset =="
./scripts/prepare-device-power-root.sh

rmmod hd60prodrv 2>/dev/null || true
./scripts/load-safe.sh ./hd60prodrv.ko \
	mmio_dump=1 \
	enable_busmaster=1 \
	request_irq_vector=1 \
	irq_mode="${IRQ_MODE:-auto}" \
	enable_v4l2=1 \
	prepare_dma_buffers=1 \
	force_32bit_dma=1 \
	synthetic_v4l2=0 \
	allow_dma_capture=1 \
	allow_mailbox_writes=1 \
	allow_preinit_command1=1 \
	allow_firmware_load=1 \
	firmware_load_mode="${FIRMWARE_LOAD_MODE:-base}" \
	firmware_base_selector="${FIRMWARE_BASE_SELECTOR:-1}" \
	firmware_name="${FIRMWARE_NAME:-hd60prodrv/MZ0380.HD.HEX}" \
	mailbox_bar=0 \
	${EXTRA_ARGS:-}

DBG="/sys/kernel/debug/hd60prodrv/0000:22:00.0"
SYS="/sys/bus/pci/devices/0000:22:00.0"

echo
echo "== pci power state =="
for f in power/control power/runtime_status power/runtime_suspended_time power/runtime_active_time d3cold_allowed; do
	if [ -r "$SYS/$f" ]; then
		printf "%s: " "$f"
		cat "$SYS/$f"
	fi
done

echo
echo "== windows_preinit_state before =="
cat "$DBG/windows_preinit_state"

echo
echo "== preinit_command1 =="
set +e
timeout "$PREINIT_TIMEOUT" cat "$DBG/preinit_command1" >"$TMPDIR/preinit.txt"
PREINIT_STATUS=$?
set -e
cat "$TMPDIR/preinit.txt"

echo
echo "== preinit summary =="
grep -E '^(result|classification|attempts_run|success_count|timeout_count|enodev_count|total_irq_delta|max_irq_delta|first_irq_delta_attempt|first_nonzero_irq_status_attempt|first_nonzero_irq_status|first_completion_change_attempt|first_completion_change|final_windows_ack_sequence|final_doorbell_bar0_000|final_completion_bar0_02c|final_arg0_bar0_008|final_arg1_bar0_00c):' "$TMPDIR/preinit.txt" || true

echo
echo "== windows_preinit_state after preinit =="
cat "$DBG/windows_preinit_state"

if [ "$PREINIT_STATUS" -ne 0 ]; then
	echo
	echo "result: preinit_command1 command timed out or was interrupted by timeout(1)"
	echo "final_verdict: PREINIT_TIMEOUT"
	rmmod hd60prodrv
	exit 10
fi

if grep -q '^result: 0$' "$TMPDIR/preinit.txt"; then
	echo
	echo "== firmware_load ${FIRMWARE_LOAD_MODE:-base} =="
	set +e
	timeout "$FIRMWARE_LOAD_TIMEOUT" cat "$DBG/firmware_load" >"$TMPDIR/firmware-load.txt"
	FW_STATUS=$?
	set -e
	cat "$TMPDIR/firmware-load.txt"

	echo
	echo "== firmware_load summary =="
	grep -E '^(firmware_name|firmware_load_mode|firmware_size|prepare_command|prepare_result|prepare_completion|prepare_irq_delta|copied_bytes|commit_command|commit_result|commit_completion|commit_irq_delta|windows_success_condition_bar_.*_0x08_eq_0|classification|result):' "$TMPDIR/firmware-load.txt" || true
else
	echo
	echo "firmware_load skipped: preinit_command1 did not complete"
	echo "final_verdict: PREINIT_FAILED"
	rmmod hd60prodrv
	exit 11
fi

if [ "$FW_STATUS" -ne 0 ]; then
	echo
	echo "result: firmware_load command timed out or was interrupted by timeout(1)"
	echo "final_verdict: FIRMWARE_LOAD_TIMEOUT"
	rmmod hd60prodrv
	exit 12
fi

if ! grep -q '^result: 0$' "$TMPDIR/firmware-load.txt"; then
	echo
	echo "final_verdict: FIRMWARE_LOAD_FAILED"
	rmmod hd60prodrv
	exit 13
fi

echo
echo "== V4L2 devices =="
v4l2-ctl --list-devices 2>/dev/null || true

echo
echo "== capture_info before stream =="
cat "$DBG/capture_info"

rm -f "$OUT"
echo
echo "== capture $FRAMES frame(s) from $DEVICE =="
v4l2-ctl -d "$DEVICE" --stream-mmap=4 --stream-count="$FRAMES" --stream-to="$OUT"

echo
echo "== analyze raw YUYV =="
set +e
python3 - "$OUT" "$FRAME_BYTES" <<'PY'
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
    diff = sum(1 for off, b in enumerate(sample) if b != fallback[off & 3])
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
echo "== capture_info after stream =="
cat "$DBG/capture_info"

if [ "$ANALYZE_STATUS" -eq 0 ]; then
	echo
	echo "final_verdict: REAL_FRAME_CANDIDATE"
	rmmod hd60prodrv
	exit 0
fi

echo
echo "final_verdict: FALLBACK_ONLY"
rmmod hd60prodrv
exit "$ANALYZE_STATUS"
