#!/usr/bin/env bash
# Build a Crucible AppImage: one file, marked executable, double-clicked.
#
# The one-line installer compiles from source, which is the right default for a
# program that also compiles its own compute backends -- but it wants a
# toolchain, twenty minutes and a terminal, and none of those are things to ask
# of somebody who just wants to look at the thing. An AppImage is the Linux
# answer to that: a single file that runs where it is downloaded, with no
# install step, no root, and nothing left behind when it is deleted.
#
#   packaging/linux/appimage.sh <installed-prefix> <output.AppImage>
#
# The prefix is one `cmake --install build --component crucible` has already
# written -- bin/crucible plus lib/crucible/*.so -- because the layout inside the
# AppDir is the install layout, unchanged. That is the whole trick: the binary's
# RPATH is $ORIGIN/../lib/crucible, so a prefix copied into usr/ finds its
# libraries with nothing patched and nothing set in the environment.
set -euo pipefail

PREFIX="${1:?usage: appimage.sh <installed-prefix> <output.AppImage>}"
OUTPUT="${2:?usage: appimage.sh <installed-prefix> <output.AppImage>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

[ -x "$PREFIX/bin/crucible" ] || { echo "no crucible in $PREFIX/bin" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
APPDIR="$WORK/Crucible.AppDir"

mkdir -p "$APPDIR/usr" "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/256x256/apps"
cp -a "$PREFIX/bin" "$APPDIR/usr/bin"
cp -a "$PREFIX/lib" "$APPDIR/usr/lib"

# GLFW is the one library that may be the system's rather than Crucible's, and
# an AppImage that needs a -dev package installed is not an AppImage. Copy it in
# beside llama.cpp's, which is on the binary's RPATH.
if glfw="$(ldd "$PREFIX/bin/crucible" | awk '/libglfw/ {print $3}')" && [ -n "$glfw" ]; then
    cp -L "$glfw" "$APPDIR/usr/lib/crucible/"
fi

# Exec is a bare name here, unlike the installed entry: AppRun puts the right
# directory on the path before anything reads it.
sed -e 's|@CRUCIBLE_GUI_EXEC@|crucible|' "$ROOT/packaging/crucible.desktop.in" \
    > "$APPDIR/crucible.desktop"
cp "$APPDIR/crucible.desktop" "$APPDIR/usr/share/applications/crucible.desktop"
cp "$ROOT/packaging/crucible.png" "$APPDIR/crucible.png"
cp "$ROOT/packaging/crucible.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/crucible.png"

# AppRun is what the outer file runs, and it resolves its own path rather than
# trusting the caller's: the AppImage is mounted somewhere different on every
# launch, and the one thing this script must not do is guess where usr/bin is.
# Crucible writes nothing next to itself -- config, models and history are all
# under the user's own directories -- so there is nothing else to arrange.
cat > "$APPDIR/AppRun" <<'RUN'
#!/bin/sh
APPDIR="$(dirname "$(readlink -f "$0")")"
exec "$APPDIR/usr/bin/crucible" "$@"
RUN
chmod +x "$APPDIR/AppRun"

# appimagetool itself is an AppImage, and on a machine with no FUSE it cannot
# mount itself -- which is every container and most CI runners. Extracting it
# and running the contents is the documented way through that, and it is what
# this does rather than asking for a kernel module.
TOOL="${APPIMAGETOOL:-}"
if [ -z "$TOOL" ]; then
    TOOL="$WORK/appimagetool"
    echo "fetching appimagetool"
    curl -fsSL -o "$TOOL" \
        https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
    chmod +x "$TOOL"
fi

mkdir -p "$(dirname "$OUTPUT")"
ARCH=x86_64 "$TOOL" --appimage-extract-and-run "$APPDIR" "$OUTPUT"
chmod +x "$OUTPUT"
echo "wrote $OUTPUT"
