#!/bin/sh
set -eu

ROOT="${1:-vendor/elgato-hd60pro-1.1.0.194}"

if [ ! -d "$ROOT" ]; then
	echo "missing $ROOT; run ./scripts/extract-elgato-driver.sh first" >&2
	exit 1
fi

for fw in MZ0380.HD.HEX MZ0380.SD.HEX MZ0381.HD.HEX MZ0381.SD.HEX; do
	src="$ROOT/extracted/$fw"
	tarfile="$ROOT/firmware/$fw.tar"
	[ -r "$src" ] || continue

	echo "== $fw =="
	stat -c 'size: %s bytes' "$src"
	sha256sum "$src" | awk '{print "sha256: " $1}'
	if [ -r "$tarfile" ]; then
		echo "tar_entries: $(tar -tf "$tarfile" | wc -l)"
		tar -tf "$tarfile" | grep -E 'FW\.TXT|BASE\.TXT|check_list$' | while IFS= read -r p; do
			printf '%s: ' "$p"
			tar -xOf "$tarfile" "$p" 2>/dev/null | tr '\n' ' ' || true
			printf '\n'
		done
	fi
	echo
done
