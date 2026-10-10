#!/usr/bin/env bash
# Build Crucible.app and wrap it in a disk image -- the Mac way to install
# something, which is to drag it into Applications.
#
#   packaging/macos/dmg.sh <installed-prefix> <output.dmg> [version]
#
# Given an output ending in .app instead, it stops at the bundle and puts it
# there: how to open the bundle a release will ship, icon and signature and
# all, without making an image to get at it.
#
# The bundle this makes is self-contained, unlike the one install.sh writes.
# That one is a launcher: three lines of shell that exec the binary the source
# install put in a prefix, because there the program is already on the disk and
# a second copy would only go stale. Here there is no prefix and nothing else on
# the machine, so the binary and llama.cpp's dylibs go inside the bundle.
#
# The layout is the install layout moved under Contents/, which is deliberate:
# the binary's RPATH is @loader_path/../lib/crucible, so Contents/MacOS/Crucible
# finds Contents/lib/crucible/libllama.dylib with nothing patched. An
# install_name_tool pass over a bundle is the usual way to do this and it is the
# usual way it breaks; not needing one is better than getting one right.
set -euo pipefail

PREFIX="${1:?usage: dmg.sh <installed-prefix> <output.dmg|output.app> [version]}"
OUTPUT="${2:?usage: dmg.sh <installed-prefix> <output.dmg|output.app> [version]}"
VERSION="${3:-0.8.9}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

[ -x "$PREFIX/bin/crucible" ] || { echo "no crucible in $PREFIX/bin" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/stage"
APP="$STAGE/Crucible.app"

mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/lib"
cp "$PREFIX/bin/crucible" "$APP/Contents/MacOS/Crucible"
# The window a program Crucible makes runs in: copied out of here into each
# app a person makes. See tools/app_runner.cpp.
if [ -x "$PREFIX/bin/crucible-app" ]; then
    cp "$PREFIX/bin/crucible-app" "$APP/Contents/MacOS/crucible-app"
fi
cp -a "$PREFIX/lib/crucible" "$APP/Contents/lib/crucible"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>Crucible</string>
    <key>CFBundleDisplayName</key><string>Crucible</string>
    <key>CFBundleIdentifier</key><string>dev.crucible.app</string>
    <key>CFBundleVersion</key><string>${VERSION}</string>
    <key>CFBundleShortVersionString</key><string>${VERSION}</string>
    <key>CFBundleExecutable</key><string>Crucible</string>
    <key>CFBundleIconFile</key><string>crucible</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>LSMinimumSystemVersion</key><string>11.0</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST

# An .icns with the sizes the Finder actually asks for. iconutil is part of
# macOS; sips alone produces a single-resolution file that looks soft in the
# dock, which is the one place this icon is going to be seen.
#
# The Mac has an icon of its own, drawn to Apple's grid -- the tile inset from
# the edge of the canvas, with its shadow -- because the dock sets icons side
# by side and one drawn edge to edge looks a size too big among them.
ICON="$ROOT/packaging/icons/crucible-mac.png"
[ -f "$ICON" ] || ICON="$ROOT/packaging/icons/crucible.png"
if [ -f "$ICON" ]; then
    set="$WORK/crucible.iconset"
    mkdir -p "$set"
    for size in 16 32 64 128 256 512; do
        sips -z "$size" "$size" "$ICON" \
             --out "$set/icon_${size}x${size}.png" >/dev/null
        sips -z "$((size * 2))" "$((size * 2))" "$ICON" \
             --out "$set/icon_${size}x${size}@2x.png" >/dev/null
    done
    iconutil -c icns "$set" -o "$APP/Contents/Resources/crucible.icns"
fi

# Signed ad hoc, which needs no Apple certificate and is not the same as
# unsigned. On Apple Silicon every piece of code has to carry a signature, and
# an application's bundle has to be sealed by one: the binary and the dylibs
# arrive here with the linker's own signatures, which say nothing about the
# bundle around them -- and anything that touched a binary after the link
# (an install step rewriting a library path) left that signature invalid.
# Downloaded and quarantined, a bundle like that is "damaged" as far as
# Gatekeeper is concerned, and "damaged" has no Open Anyway. Sealed, it is an
# application from an unidentified developer, which Privacy & Security will
# open once you say so.
#
# Inside out: each library first, then the bundle, which seals them.
find "$APP/Contents/lib" -type f \( -name '*.dylib' -o -name '*.so' \) -print0 |
    while IFS= read -r -d '' library; do
        codesign --force --sign - --timestamp=none "$library"
    done
if [ -f "$APP/Contents/MacOS/crucible-app" ]; then
    codesign --force --sign - --timestamp=none "$APP/Contents/MacOS/crucible-app"
fi
codesign --force --sign - --timestamp=none "$APP/Contents/MacOS/Crucible"
codesign --force --sign - --timestamp=none "$APP"
codesign --verify --deep --strict --verbose=2 "$APP"

if [ "${OUTPUT%.app}" != "$OUTPUT" ]; then
    mkdir -p "$(dirname "$OUTPUT")"
    rm -rf "$OUTPUT"
    ditto "$APP" "$OUTPUT"
    echo "wrote $OUTPUT"
    exit 0
fi

# What a downloader meets is still a refusal the first time, so the image
# carries the way past it.
ln -s /Applications "$STAGE/Applications"
cat > "$STAGE/Read me first.txt" <<'NOTE'
Crucible

  Drag Crucible into Applications, then open it from there.

  The first time, macOS will refuse: Crucible is not signed with an Apple
  developer certificate, so macOS cannot check it. To open it anyway:

    1. Open Crucible once and dismiss the message.
    2. Open System Settings, then Privacy & Security.
    3. Near the bottom, beside "Crucible was blocked", choose Open Anyway,
       and confirm.

  That is a one-time answer. (On macOS 14 and earlier, right-clicking
  Crucible in Applications and choosing Open does the same.)

  Everything Crucible does happens on this machine. It ships no models and
  downloads none by itself.
NOTE

mkdir -p "$(dirname "$OUTPUT")"
rm -f "$OUTPUT"
hdiutil create -volname "Crucible" -srcfolder "$STAGE" -ov -format UDZO "$OUTPUT"
echo "wrote $OUTPUT"
