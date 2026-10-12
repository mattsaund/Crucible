#!/bin/sh
# Make an application's .icns from a square PNG.
#
#   packaging/macos/icns.sh <icon.png> <out.icns>
#
# Used by both roads to a Mac application: dmg.sh, for the self-contained one
# a release ships, and install.sh, for the launcher a source install makes.
#
# `sips -s format icns` alone cannot do it from a 1024-pixel PNG -- it fails,
# and the installer that asked it to, and hid the failure, made an application
# with no icon at all. An iconset of the sizes the Finder asks for, each made
# by sips, packed by iconutil, is what works: one picture per size, sharp in
# the dock. Both tools are part of macOS.
set -eu

ICON="${1:?usage: icns.sh <icon.png> <out.icns>}"
OUT="${2:?usage: icns.sh <icon.png> <out.icns>}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SET="$WORK/crucible.iconset"
mkdir -p "$SET"
for size in 16 32 64 128 256 512; do
    sips -z "$size" "$size" "$ICON" --out "$SET/icon_${size}x${size}.png" >/dev/null
    sips -z "$((size * 2))" "$((size * 2))" "$ICON" --out "$SET/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$SET" -o "$OUT"
