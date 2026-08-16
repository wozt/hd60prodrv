#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root" >&2
	exit 1
fi

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$ROOT"

./scripts/test-post-logo-cmd1d-root.sh

echo
echo "initialized: pipeline-ready driver is loaded and /dev/video0 is available in synthetic mode"
