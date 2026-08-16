#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root" >&2
	exit 1
fi

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$ROOT"

./scripts/install-logo-payloads-root.sh
rmmod hd60prodrv 2>/dev/null || true
./scripts/load-safe.sh ./hd60prodrv.ko mmio_dump=1 mailbox_bar=0

DBG="/sys/kernel/debug/hd60prodrv/0000:22:00.0"

echo "== logo_upload_plan =="
timeout 10s cat "$DBG/logo_upload_plan"

echo
echo "== health =="
timeout 10s cat "$DBG/health"

echo
echo "== dmesg =="
dmesg | grep hd60prodrv | tail -40
