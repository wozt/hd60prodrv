#!/bin/sh
set -eu

BDF="${1:-0000:22:00.0}"
OUT="${2:-snapshots/$(date +%Y%m%d-%H%M%S)-$BDF}"
DBG="/sys/kernel/debug/hd60prodrv/$BDF"
SYS="/sys/bus/pci/devices/$BDF"

mkdir -p "$OUT"

echo "writing snapshot to $OUT"

uname -a >"$OUT/uname.txt"
lspci -nn -s "$BDF" -vvv >"$OUT/lspci-vvv.txt" 2>&1 || true

for f in vendor device subsystem_vendor subsystem_device class irq numa_node resource; do
	if [ -r "$SYS/$f" ]; then
		cat "$SYS/$f" >"$OUT/sysfs-$f.txt"
	fi
done

if [ -r "$SYS/config" ]; then
	if command -v xxd >/dev/null 2>&1; then
		xxd -g1 -l 256 "$SYS/config" >"$OUT/sysfs-config-256.txt" || true
	else
		od -Ax -tx1 -N 256 "$SYS/config" >"$OUT/sysfs-config-256.txt" || true
	fi
fi

if [ -d "$DBG" ]; then
	for f in info health firmware_info pci_config bar5_head bar5_regs fw_version; do
		if [ -r "$DBG/$f" ]; then
			if command -v timeout >/dev/null 2>&1; then
				timeout 3s cat "$DBG/$f" >"$OUT/debugfs-$f.txt" 2>"$OUT/debugfs-$f.err" || true
			else
				cat "$DBG/$f" >"$OUT/debugfs-$f.txt" || true
			fi
		fi
	done
else
	echo "$DBG not present; load hd60prodrv first" >"$OUT/debugfs-missing.txt"
fi

dmesg | grep hd60prodrv >"$OUT/dmesg-hd60prodrv.txt" 2>&1 || true
