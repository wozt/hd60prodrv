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
	allow_mailbox_writes=1 \
	allow_preinit_command1=1 \
	allow_fw_status_command10=1 \
	request_irq_vector=1 \
	irq_mode=auto \
	mailbox_bar=0 \
	enable_v4l2=0

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
grep -E '^(result|classification|attempts_run|success_count|timeout_count|enodev_count|total_irq_delta|max_irq_delta|first_irq_delta_attempt|first_nonzero_irq_status_attempt|first_nonzero_irq_status|first_completion_change_attempt|first_completion_change|final_doorbell_bar0_000|final_completion_bar0_02c|final_arg0_bar0_008|final_arg1_bar0_00c):' "$TMPDIR/preinit.txt" || true

echo
echo "== windows_preinit_state after preinit =="
cat "$DBG/windows_preinit_state"

if grep -q '^result: 0$' "$TMPDIR/preinit.txt"; then
	echo
	echo "== fw_status_command10 =="
	timeout 40s cat "$DBG/fw_status_command10" | tee "$TMPDIR/fw_status.txt"

	echo
	echo "== windows_init_plan =="
	cat "$DBG/windows_init_plan"
else
	echo
	echo "== fw_status_command10 skipped =="
	echo "preinit_command1 did not complete; not sending the next Windows command"
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
