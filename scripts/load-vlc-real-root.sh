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

if [ "${INIT_FIRST:-1}" != "0" ]; then
	echo "== prepare power, no PCI reset =="
	./scripts/prepare-device-power-root.sh
else
	./scripts/pci-preflight-root.sh "${BDF:-0000:22:00.0}"
fi

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

if [ "${INIT_FIRST:-1}" != "0" ]; then
	echo
	echo "== preinit_command1 =="
	timeout "${PREINIT_TIMEOUT:-65s}" cat "$DBG/preinit_command1" | tee "$TMPDIR/preinit.txt"

	if grep -q '^result: 0$' "$TMPDIR/preinit.txt"; then
		echo
		echo "== firmware_load ${FIRMWARE_LOAD_MODE:-base} =="
		timeout "${FIRMWARE_LOAD_TIMEOUT:-220s}" cat "$DBG/firmware_load" | tee "$TMPDIR/firmware-load.txt"
	else
		echo
		echo "firmware_load skipped: preinit_command1 did not complete"
	fi
else
	echo
	echo "INIT_FIRST=0: skipping preinit_command1/firmware_load"
fi

echo
echo "V4L2 devices:"
v4l2-ctl --list-devices 2>/dev/null || true
echo
echo "Open with VLC, adjusting the node if another camera owns /dev/video0:"
echo "  vlc v4l2:///dev/video0"
echo
echo "Optional exact Windows extra packets can be tested by passing EXTRA_ARGS, for example:"
echo "  EXTRA_ARGS='allow_stream_extra_commands=1 send_stream_extra_commands=1 stream_extra_primary_2d=0x800,0x2d,...' sudo -E $0"
