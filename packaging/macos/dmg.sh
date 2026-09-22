#!/usr/bin/env bash
# Build Crucible.app and wrap it in a disk image -- the Mac way to install
# something, which is to drag it into Applications.
#
#   packaging/macos/dmg.sh <installed-prefix> <output.dmg> [version]
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

PREFIX="${1:?usage: dmg.sh <installed-prefix> <output.dmg> [version]}"
OUTPUT="${2:?usage: dmg.sh <installed-prefix> <output.dmg> [version]}"
VERSION="${3:-0.5.0}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

[ -x "$PREFIX/bin/crucible" ] || { echo "no crucible in $PREFIX/bin" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/stage"
APP="$STAGE/Crucible.app"

mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/lib"
cp "$PREFIX/bin/crucible" "$APP/Contents/MacOS/Crucible"
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
if [ -f "$ROOT/packaging/crucible.png" ]; then
    set="$WORK/crucible.iconset"
    mkdir -p "$set"
    for size in 16 32 64 128 256 512; do
        sips -z "$size" "$size" "$ROOT/packaging/crucible.png" \
             --out "$set/icon_${size}x${size}.png" >/dev/null
        sips -z "$((size * 2))" "$((size * 2))" "$ROOT/packaging/crucible.png" \
             --out "$set/icon_${size}x${size}@2x.png" >/dev/null
    done
    iconutil -c icns "$set" -o "$APP/Contents/Resources/crucible.icns"
fi

# Unsigned, and honest about it: without an Apple developer certificate the
# Gatekeeper quarantine is what a downloader meets, so the image carries the
# instruction for getting past it rather than leaving somebody with a dialog
# that says the application is damaged.
ln -s /Applications "$STAGE/Applications"
cat > "$STAGE/Read me first.txt" <<'NOTE'
Crucible

  Drag Crucible into Applications, then open it from there.

  The first time, macOS will refuse: Crucible is not signed with an Apple
  developer certificate, and an unsigned application downloaded from the
  internet is quarantined. Right-click it in Applications and choose Open,
  and the dialog gains an Open button. That is a one-time answer.

  Everything Crucible does happens on this machine. It ships no models and
  downloads none by itself.
NOTE

mkdir -p "$(dirname "$OUTPUT")"
rm -f "$OUTPUT"
hdiutil create -volname "Crucible" -srcfolder "$STAGE" -ov -format UDZO "$OUTPUT"
echo "wrote $OUTPUT"
