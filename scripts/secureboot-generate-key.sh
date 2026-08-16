#!/bin/sh
set -eu

KEY_DIR="${1:-certs}"
KEY="$KEY_DIR/MOK.priv"
CRT="$KEY_DIR/MOK.pem"
DER="$KEY_DIR/MOK.der"

mkdir -p "$KEY_DIR"
chmod 700 "$KEY_DIR"

if [ -e "$KEY" ] || [ -e "$CRT" ] || [ -e "$DER" ]; then
	echo "refusing to overwrite existing key material in $KEY_DIR" >&2
	exit 1
fi

openssl req -new -x509 -newkey rsa:4096 \
	-keyout "$KEY" \
	-out "$CRT" \
	-outform PEM \
	-nodes \
	-days 36500 \
	-subj "/CN=hd60prodrv local module signing/"

openssl x509 -in "$CRT" -outform DER -out "$DER"
chmod 600 "$KEY"
chmod 644 "$CRT" "$DER"

echo "created:"
echo "  private key: $KEY"
echo "  certificate: $CRT"
echo "  MOK import:  $DER"
echo
echo "next:"
echo "  sudo mokutil --import $DER"
echo "  reboot and enroll the key in MokManager"
