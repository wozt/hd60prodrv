#!/bin/sh
set -eu

MODULE="${1:-./hd60prodrv.ko}"
KEY="${2:-certs/MOK.priv}"
CRT="${3:-certs/MOK.pem}"
KREL="$(uname -r)"
KBUILD="${KREL%%-amd64}"
SIGN_FILE="/usr/lib/linux-kbuild-$KBUILD/scripts/sign-file"

if [ ! -r "$MODULE" ]; then
	echo "module not found: $MODULE" >&2
	exit 1
fi

if [ ! -r "$KEY" ] || [ ! -r "$CRT" ]; then
	echo "missing key/certificate; run ./scripts/secureboot-generate-key.sh first" >&2
	exit 1
fi

if [ ! -x "$SIGN_FILE" ]; then
	SIGN_FILE="$(find /usr/lib -path "*/scripts/sign-file" -type f | sort -V | tail -1)"
fi

if [ ! -x "$SIGN_FILE" ]; then
	echo "sign-file not found; install linux-kbuild for the running kernel" >&2
	exit 1
fi

"$SIGN_FILE" sha256 "$KEY" "$CRT" "$MODULE"
if ! grep -aq "Module signature appended" "$MODULE"; then
	echo "sign-file returned success, but no module signature marker was found in $MODULE" >&2
	exit 1
fi
echo "signed $MODULE with $CRT"
