#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Fetch the 2016 Memblaze PBlaze3 vendor driver for reference.
#
# This project deliberately does not redistribute the vendor sources.
# The upstream repository carries a MODULE_LICENSE("GPL") notice in the
# driver files but ships no COPYING/LICENSE file, so the terms are not
# stated unambiguously; fetch it yourself and read it under its own
# terms. Nothing in this repository is built from or includes these
# files -- they are reference material for PROTOCOL.md.
#
# Usage: ./scripts/fetch-vendor-driver.sh [dest]

set -eu

REPO="https://github.com/colyli/misc-tools"
COMMIT="c57e728e55d36074413f934a8d49a20c12ea8970"   # master tip; pblaze3-driver content pinned here
DEST="${1:-vendor/misc-tools}"

if [ -e "$DEST" ]; then
	echo "$DEST already exists; remove it first" >&2
	exit 1
fi

echo "fetching $REPO at $COMMIT into $DEST ..."
mkdir -p "$DEST"
git -C "$DEST" init -q
git -C "$DEST" remote add origin "$REPO"
git -C "$DEST" fetch -q --depth 1 origin "$COMMIT"
git -C "$DEST" checkout -q FETCH_HEAD

echo "done. vendor driver at: $DEST/pblaze3-driver/"
echo
echo "note: upstream ships no LICENSE file. The driver sources carry"
echo "      MODULE_LICENSE(\"GPL\") and MODULE_AUTHOR(\"MemBlaze\")."
echo "      Treat these files under their own terms; do not relicense."
