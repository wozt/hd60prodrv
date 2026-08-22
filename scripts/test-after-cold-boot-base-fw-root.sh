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
timeout 65s cat "$DBG/preinit_command1" | tee "$TMPDIR/preinit.txt"

echo
echo "== preinit summary =="
grep -E '^(result|classification|attempts_run|success_count|timeout_count|enodev_count|total_irq_delta|max_irq_delta|first_irq_delta_attempt|first_nonzero_irq_status_attempt|first_nonzero_irq_status|first_completion_change_attempt|first_completion_change|final_windows_ack_sequence|final_doorbell_bar0_000|final_completion_bar0_02c|final_arg0_bar0_008|final_arg1_bar0_00c):' "$TMPDIR/preinit.txt" || true

echo
echo "== windows_preinit_state after preinit =="
cat "$DBG/windows_preinit_state"

if grep -q '^result: 0$' "$TMPDIR/preinit.txt"; then
	echo
	echo "== firmware_load base 0x0e/0x0f =="
	timeout 220s cat "$DBG/firmware_load" | tee "$TMPDIR/base-fw.txt"

	echo
	echo "== firmware_load summary =="
	grep -E '^(firmware_name|firmware_load_mode|firmware_size|prepare_command|prepare_result|prepare_completion|prepare_irq_delta|copied_bytes|commit_command|commit_result|commit_completion|commit_irq_delta|windows_success_condition_bar_.*_0x08_eq_0|classification|result):' "$TMPDIR/base-fw.txt" || true
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
