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

echo "== prepare power, no PCI reset =="
./scripts/prepare-device-power-root.sh

rmmod hd60prodrv 2>/dev/null || true
./scripts/load-safe.sh ./hd60prodrv.ko \
	mmio_dump=1 \
	enable_v4l2=0 \
	mailbox_bar=0 \
	request_irq_vector=1 \
	irq_mode=auto \
	allow_mailbox_writes=1 \
	allow_preinit_command1=1 \
	allow_firmware_load=1 \
	firmware_load_mode=base \
	firmware_base_selector=1 \
	firmware_name=hd60prodrv/MZ0380.HD.HEX

DBG="/sys/kernel/debug/hd60prodrv/0000:22:00.0"

echo
echo "== windows_preinit_state before =="
cat "$DBG/windows_preinit_state"

echo
echo "== preinit_command1 =="
timeout 65s cat "$DBG/preinit_command1" | tee "$TMPDIR/preinit.txt"

echo
echo "== windows_preinit_state after preinit =="
cat "$DBG/windows_preinit_state"

if grep -q '^result: 0$' "$TMPDIR/preinit.txt"; then
	echo
	echo "== firmware_load base 0x0e/0x0f =="
	timeout 220s cat "$DBG/firmware_load" | tee "$TMPDIR/base-fw.txt"
else
	echo
	echo "== firmware_load skipped =="
	echo "preinit_command1 did not complete; not sending base firmware commands"
fi

echo
echo "== health =="
cat "$DBG/health"

echo
echo "== mailbox_regs =="
cat "$DBG/mailbox_regs"

echo
echo "== bar5_regs =="
cat "$DBG/bar5_regs"

rmmod hd60prodrv
