#!/bin/sh
set -eu

URL="${1:-https://edge.elgato.com/egc/windows/drivers/hd60-pro/Game_Capture_HD60_Pro_1.1.0.194.exe}"
OUT="${2:-vendor/elgato-hd60pro-1.1.0.194}"
EXE="$OUT/driver.exe"

mkdir -p "$OUT/extracted" "$OUT/firmware"

if [ ! -r "$EXE" ]; then
	curl -L -o "$EXE" "$URL"
fi

7z x -y -o"$OUT/extracted" "$EXE" >/dev/null

for fw in MZ0380.HD.HEX MZ0380.SD.HEX MZ0381.HD.HEX MZ0381.SD.HEX; do
	if [ -r "$OUT/extracted/$fw" ]; then
		gzip -cd "$OUT/extracted/$fw" >"$OUT/firmware/$fw.tar"
	fi
done

echo "extracted to $OUT"
echo "firmware tars:"
find "$OUT/firmware" -maxdepth 1 -type f -printf '  %p\n' | sort
