#!/bin/sh
set -eu

BDF="${1:-0000:22:00.0}"
DEV="/sys/bus/pci/devices/$BDF"

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo $0 [BDF]" >&2
	exit 1
fi

if lsmod | grep -q '^hd60prodrv\b'; then
	rmmod hd60prodrv
fi

reset_ok=0

echo "warning: PCI reset can leave this 12ab:0380 card mailbox-deaf."
echo "warning: prefer scripts/prepare-device-power-root.sh, then a full power cycle if the mailbox is still dead."

if [ -w "$DEV/power/control" ]; then
	sh -c "echo on > '$DEV/power/control'" 2>/dev/null || true
fi

if [ -w "$DEV/d3cold_allowed" ]; then
	sh -c "echo 0 > '$DEV/d3cold_allowed'" 2>/dev/null || true
fi

if [ -w "$DEV/reset" ]; then
	echo "resetting $BDF through PCI reset"
	if sh -c "echo 1 > '$DEV/reset'" 2>/dev/null; then
		reset_ok=1
	else
		echo "PCI reset failed; trying remove/rescan" >&2
	fi
fi

if [ "$reset_ok" -eq 0 ] && [ -w "$DEV/remove" ]; then
	echo "removing and rescanning $BDF"
	echo 1 >"$DEV/remove"
	sleep 1
	echo 1 >/sys/bus/pci/rescan
	reset_ok=1
fi

if [ "$reset_ok" -eq 0 ]; then
	echo "no PCI reset/remove control available for $BDF" >&2
	exit 1
fi

sleep 1
if ! lspci -nn -s "$BDF" -vvv; then
	echo "$BDF did not reappear on the PCI bus" >&2
	exit 1
fi
