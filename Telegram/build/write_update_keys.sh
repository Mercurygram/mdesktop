#!/usr/bin/env bash
#
# This file is part of Mercurygram Desktop,
# an unofficial privacy/security focused fork of Telegram Desktop.
#
# Writes the update-signing private keys into a DesktopPrivate directory as the
# C headers packer.cpp expects (PrivateKey / PrivateBetaKey in packer_private.h,
# plus an empty AlphaPrivateKey since the closed-alpha path stays disabled).
#
# The keys are read from the environment so they never touch the command line:
#   UPDATE_PRIVATE_KEY        stable-channel signing key (PEM)
#   UPDATE_PRIVATE_BETA_KEY   beta-channel signing key (PEM)
#
# Usage: write_update_keys.sh <dest-dir>
#
# Shared by every platform job in .github/workflows/release.yml so the PEM ->
# C-string encoding lives in exactly one place.
set -e

dest="$1"
if [ -z "$dest" ]; then
	echo "usage: write_update_keys.sh <dest-dir>" >&2
	exit 1
fi
mkdir -p "$dest"

pem_to_cstr() { # $1=varname; PEM on stdin
	echo "const char *$1 = \"\\"
	grep -v '^[[:space:]]*$' | sed 's/[[:space:]]*$//; s/$/\\n\\/'
	echo "\";"
}

{
	printf '%s\n' "$UPDATE_PRIVATE_KEY" | pem_to_cstr PrivateKey
	printf '%s\n' "$UPDATE_PRIVATE_BETA_KEY" | pem_to_cstr PrivateBetaKey
} > "$dest/packer_private.h"
echo 'const char *AlphaPrivateKey = "";' > "$dest/alpha_private.h"
