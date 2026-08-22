#!/bin/sh
set -eu

BDF="${1:-0000:22:00.0}"
DEV="/sys/bus/pci/devices/$BDF"

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo $0 [BDF]" >&2
	exit 1
fi

if [ ! -d "$DEV" ]; then
	echo "$BDF is not present under /sys/bus/pci/devices" >&2
	exit 1
fi

"$(dirname -- "$0")/pci-preflight-root.sh" "$BDF"

if [ -w "$DEV/power/control" ]; then
	echo on >"$DEV/power/control"
fi

if [ -w "$DEV/d3cold_allowed" ]; then
	echo 0 >"$DEV/d3cold_allowed"
fi

echo "prepared $BDF power management without PCI reset"
if [ -r "$DEV/power/control" ]; then
	printf "power/control: "
	cat "$DEV/power/control"
fi
if [ -r "$DEV/d3cold_allowed" ]; then
	printf "d3cold_allowed: "
	cat "$DEV/d3cold_allowed"
fi
