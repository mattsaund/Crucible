#!/usr/bin/env bash
# A runtime of Crucible on its own: the program built with one llama.cpp, for
# Settings, Runtimes to fetch without a new Crucible. The other half is
# src/runtime/llama_versions.cpp, which unpacks it into
# runtimes/core/<version>/<llama tag>/ for the launcher to load.
#
#   packaging/runtime/core.sh <cmake-build-dir> <output-dir>
#
# Writes runtime-core-<platform>-<version>-<llama tag>.tar.gz into the output
# folder: the program as a module, the window's and the engine's, llama.cpp's
# libraries beside them, and runtime.json, which says which Crucible and which
# llama.cpp it is. The compute backends are packed by runtimes.yml.
set -euo pipefail

BUILD="${1:?usage: core.sh <build-dir> <output-dir>}"
OUT="${2:?usage: core.sh <build-dir> <output-dir>}"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK:?}"' EXIT
STAGE="$WORK/stage"
CORE="$WORK/core"
mkdir -p "$CORE"

cmake --install "$BUILD" --config Release --component crucible --prefix "$STAGE" > /dev/null
cmake --install "$BUILD" --config Release --component cli --prefix "$STAGE" > /dev/null

case "$(uname -s)" in
    Linux)
        case "$(uname -m)" in
            x86_64) platform=linux-x64 ;;
            aarch64) platform=linux-arm64 ;;
            *) echo "no runtime is published for $(uname -m)" >&2; exit 1 ;;
        esac
        cp -a "$STAGE/lib/crucible/." "$CORE/"
        # OpenMP's runtime, which a minimal server does not have, as the
        # terminal download carries it (packaging/cli/package.sh).
        gomp="$(ldd "$CORE/libcrucible-engine-program.so" | awk '/libgomp/ {print $3}')"
        if [ -n "$gomp" ]; then
            cp -L "$gomp" "$CORE/"
        fi
        ;;
    Darwin)
        case "$(uname -m)" in
            arm64) platform=macos-arm64 ;;
            *) platform=macos-x64 ;;
        esac
        cp -a "$STAGE/lib/crucible/." "$CORE/"
        # Installing rewrites each file's paths, which breaks the signature
        # the linker gave it, and Apple Silicon will not load an unsigned file.
        find "$CORE" -type f \( -name '*.dylib' -o -name '*.so' \) -print0 |
            xargs -0 codesign --force --sign - --timestamp=none
        ;;
    MINGW* | MSYS* | CYGWIN*)
        platform=windows-x64
        # Windows keeps everything in bin/: the libraries, not the launchers.
        find "$STAGE/bin" -maxdepth 1 -type f -iname '*.dll' -exec cp {} "$CORE/" \;
        cp "$STAGE/bin/llama-tag" "$CORE/"
        ;;
    *)
        echo "no runtime is published for $(uname -s)" >&2
        exit 1
        ;;
esac

# Run in a folder of its own, so this reads nothing of whoever runs it.
mkdir -p "$WORK/home"
quiet() {
    env HOME="$WORK/home" USERPROFILE="$WORK/home" LOCALAPPDATA="$WORK/home" APPDATA="$WORK/home" \
        XDG_CONFIG_HOME="$WORK/home/.config" XDG_DATA_HOME="$WORK/home/.local/share" \
        XDG_CACHE_HOME="$WORK/home/.cache" "$@"
}

version="$(quiet "$STAGE/bin/crucible" --version | awk 'NR==1 {print $NF}' | tr -d '\r')"
tag="$(tr -d '[:space:]' < "$CORE/llama-tag")"
case "$tag" in
    b[0-9]*) ;;
    *) echo "error: the build says its llama.cpp is \"$tag\"" >&2; exit 1 ;;
esac
printf '{"crucible": "%s", "llama_tag": "%s", "platform": "%s"}\n' "$version" "$tag" "$platform" \
    > "$CORE/runtime.json"

# It loads on its own: a launcher with nothing beside it, given this folder,
# says this llama.cpp. One that would not load leaves the launcher nothing.
# The two launchers alone, not the programs beside them on Windows.
mkdir -p "$WORK/alone"
for name in crucible crucible-engine; do
    for file in "$STAGE/bin/$name" "$STAGE/bin/$name.exe"; do
        if [ -f "$file" ]; then
            cp "$file" "$WORK/alone/"
        fi
    done
done
for name in crucible crucible-engine; do
    launcher="$WORK/alone/$name"
    [ -f "$launcher" ] || launcher="$WORK/alone/$name.exe"
    said="$(quiet env CRUCIBLE_RUNTIME_DIR="$CORE" "$launcher" --version | awk 'NR==2 {print $NF}' | tr -d '\r')"
    if [ "$said" != "$tag" ]; then
        echo "error: $name would not load this runtime on its own (it said \"$said\", not $tag)" >&2
        exit 1
    fi
done

name="runtime-core-$platform-$version-$tag.tar.gz"
mkdir -p "$OUT"
tar -czf "$OUT/$name" -C "$CORE" .
ls -la "$OUT/$name"
