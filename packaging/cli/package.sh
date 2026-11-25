#!/usr/bin/env bash
# The terminal download: cruciblecli, the engine it drives, and the libraries
# they need, in one archive for install-cli.sh to fetch.
#
#   packaging/cli/package.sh <cmake-build-dir> <cruciblecli> <output.tar.gz>
#
# The layout is an ordinary install: bin/crucible-engine, a launcher, and
# lib/crucible/, the engine itself as a module beside llama.cpp's libraries,
# found by its RPATH, $ORIGIN. A checksum goes beside it.
set -euo pipefail

BUILD="${1:?usage: package.sh <build-dir> <cruciblecli> <output.tar.gz>}"
CLI="${2:?usage: package.sh <build-dir> <cruciblecli> <output.tar.gz>}"
OUTPUT="${3:?usage: package.sh <build-dir> <cruciblecli> <output.tar.gz>}"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK:?}"' EXIT
STAGE="$WORK/cruciblecli"

cmake --install "$BUILD" --config Release --component cli --prefix "$STAGE" >/dev/null
install -m 755 "$CLI" "$STAGE/bin/cruciblecli"

case "$(uname -s)" in
    Linux)
        # OpenMP's runtime is not on a minimal server, and the engine will not
        # start without it. The RPATH finds this copy first. The module is
        # what links it: the launcher in bin/ links nothing of llama.cpp's.
        gomp="$(ldd "$STAGE/lib/crucible/libcrucible-engine-program.so" | awk '/libgomp/ {print $3}')"
        if [ -z "$gomp" ]; then
            echo "error: the engine does not link OpenMP, or its module is not in lib/crucible" >&2
            exit 1
        fi
        cp -L "$gomp" "$STAGE/lib/crucible/"
        ;;
    Darwin)
        # Installing rewrites each file's paths, which breaks the signature
        # the linker gave it, and Apple Silicon will not run an unsigned file.
        find "$STAGE" -type f \( -name '*.dylib' -o -name '*.so' -o -perm -u+x \) -print0 |
            xargs -0 codesign --force --sign -
        ;;
esac

# It starts, and says the version it was built as.
built="$("$STAGE/bin/crucible-engine" --version | awk 'NR==1 {print $NF}')"
said="$("$STAGE/bin/cruciblecli" --version | awk '{print $NF}')"
if [ "$built" != "$said" ]; then
    echo "error: the engine is $built and cruciblecli is $said (cli/Cargo.toml)" >&2
    exit 1
fi

# And it answers over its pipe, which is how cruciblecli talks to it. In a
# folder of its own, so that this leaves nothing in the home folder.
mkdir -p "$WORK/home"
answer="$(printf '{"id":1,"method":"ping"}\n' |
    env HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home/.config" XDG_DATA_HOME="$WORK/home/.local/share" \
        "$STAGE/bin/crucible-engine" 2>/dev/null | grep -m1 '"id":1' || true)"
case "$answer" in
    *'"ok":true'*) ;;
    *) echo "error: the engine did not answer over its pipe: $answer" >&2; exit 1 ;;
esac

mkdir -p "$(dirname "$OUTPUT")"
OUTPUT="$(cd "$(dirname "$OUTPUT")" && pwd)/$(basename "$OUTPUT")"
tar -C "$WORK" -czf "$OUTPUT" cruciblecli
(
    cd "$(dirname "$OUTPUT")"
    name="$(basename "$OUTPUT")"
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$name" > "$name.sha256"
    else
        shasum -a 256 "$name" > "$name.sha256"
    fi
)
ls -la "$OUTPUT"
