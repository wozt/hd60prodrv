#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo $0" >&2
	exit 1
fi

PIDS="$(ps -eo pid=,cmd= | awk '/cat \\/sys\\/kernel\\/debug\\/hd60prodrv/ { print $1 }')"
if [ -n "$PIDS" ]; then
	echo "killing stuck debugfs readers: $PIDS"
	kill -9 $PIDS || true
fi

if lsmod | grep -q '^hd60prodrv\b'; then
	echo "unloading hd60prodrv"
	rmmod hd60prodrv || {
		echo "rmmod failed; check remaining users with: lsmod | grep hd60prodrv" >&2
		exit 1
	}
fi

echo "ok"
