#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: $0 <snapshot-a> <snapshot-b>" >&2
	exit 1
fi

A="$1/debugfs-bar5_regs.txt"
B="$2/debugfs-bar5_regs.txt"

if [ ! -r "$A" ] || [ ! -r "$B" ]; then
	echo "missing debugfs-bar5_regs.txt in one of the snapshots" >&2
	exit 1
fi

awk '
	FNR == NR && /^0x/ { a[$1] = $2 " " $3; next }
	/^0x/ {
		if (!($1 in a)) {
			print $1, "<missing>", "->", $2, $3
		} else if (a[$1] != $2 " " $3) {
			print $1, a[$1], "->", $2, $3
		}
		seen[$1] = 1
	}
	END {
		for (k in a) {
			if (!(k in seen))
				print k, a[k], "->", "<missing>"
		}
	}
' "$A" "$B" | sort
