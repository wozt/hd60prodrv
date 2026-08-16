#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo $0" >&2
	exit 1
fi

cd "$(dirname "$0")/.."

rmmod hd60prodrv 2>/dev/null || true
./scripts/load-safe.sh ./hd60prodrv.ko \
	mmio_dump=1 \
	allow_mailbox_writes=1 \
	request_irq_vector=1 \
	mailbox_bar=0 \
	allow_preinit_command1=1 \
	allow_fw_status_command10=1 \
	allow_gpio17_sequence=1

DBG=/sys/kernel/debug/hd60prodrv/0000:22:00.0

echo "== preinit_command1 =="
timeout 10s cat "$DBG/preinit_command1"
echo
echo "== fw_status_command10 =="
timeout 10s cat "$DBG/fw_status_command10"
echo
echo "== gpio17_generic_sequence =="
timeout 15s cat "$DBG/gpio17_generic_sequence"
echo
echo "== health =="
cat "$DBG/health"
echo
echo "== mailbox_regs =="
cat "$DBG/mailbox_regs"
echo
echo "== bar5_regs =="
cat "$DBG/bar5_regs"
echo
echo "== dmesg =="
dmesg | grep hd60prodrv | tail -140
