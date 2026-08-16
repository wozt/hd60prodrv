#!/bin/sh
set -eu

echo "== secure boot =="
if command -v mokutil >/dev/null 2>&1; then
	mokutil --sb-state || true
else
	echo "mokutil not installed"
fi

echo
echo "== lockdown =="
if [ -r /sys/kernel/security/lockdown ]; then
	cat /sys/kernel/security/lockdown
else
	echo "lockdown status unavailable"
fi

echo
echo "== module signature marker =="
MODULE="${1:-./hd60prodrv.ko}"
if [ -r "$MODULE" ]; then
	tail -c 256 "$MODULE" | strings | tail -20 || true
else
	echo "module not found: $MODULE"
fi
