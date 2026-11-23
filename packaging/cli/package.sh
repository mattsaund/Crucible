#!/usr/bin/env bash
# The terminal download: cruciblecli, the engine it drives, and the libraries
# they need, in one archive for install-cli.sh to fetch.
#
#   packaging/cli/package.sh <cmake-build-dir> <cruciblecli> <output.tar.gz>
#
# The layout is an ordinary install, bin/ and lib/crucible/, because the
# engine's RPATH is $ORIGIN/../lib/crucible. A checksum goes beside it.
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
        # start without it. The RPATH finds this copy first.
        gomp="$(ldd "$STAGE/bin/crucible-engine" | awk '/libgomp/ {print $3}')"
        if [ -n "$gomp" ]; then
            cp -L "$gomp" "$STAGE/lib/crucible/"
        fi
        ;;
    Darwin)
        # Installing rewrites each file's paths, which breaks the signature
        # the linker gave it, and Apple Silicon will not run an unsigned file.
        find "$STAGE" -type f \( -name '*.dylib' -o -perm -u+x \) -print0 |
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
