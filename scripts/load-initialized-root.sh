#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root" >&2
	exit 1
fi

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$ROOT"

./scripts/test-after-cold-boot-root.sh

echo
echo "initialized: cold-boot mailbox probe completed"
echo "If preinit succeeded, continue with firmware_load/stream_start_test diagnostics before VLC."
