#!/bin/sh
# Crucible in the terminal: one line, no compiler.
#
#   curl -fsSL https://raw.githubusercontent.com/mattsaund/Crucible/main/install-cli.sh | sh
#
# Fetches the newest release's terminal download, puts it in
# ~/.local/lib/crucible-cli and `cruciblecli` in ~/.local/bin. Separate from the
# app, and they share their settings, models and chats.
#
#   ... | sh -s -- --version v1.0.2     one version
#   ... | sh -s -- --uninstall          take it off again (models and chats stay)
set -eu

REPO="mattsaund/Crucible"
HOME_DIR="${HOME:?HOME is not set}"
LIB_DIR="$HOME_DIR/.local/lib/crucible-cli"
BIN_DIR="$HOME_DIR/.local/bin"
MARK="# added by the cruciblecli installer"

say()  { printf '%s\n' "$*"; }
die()  { printf 'cruciblecli: %s\n' "$*" >&2; exit 1; }

VERSION=""
UNINSTALL=0
while [ $# -gt 0 ]; do
    case "$1" in
        --version) [ $# -ge 2 ] || die "--version needs a tag, like v1.0.2"; VERSION="$2"; shift 2 ;;
        --uninstall) UNINSTALL=1; shift ;;
        -h|--help)
            say "curl -fsSL https://raw.githubusercontent.com/mattsaund/Crucible/main/install-cli.sh | sh"
            say "  ... | sh -s -- --version v1.0.2     one version"
            say "  ... | sh -s -- --uninstall          take it off again (models and chats stay)"
            exit 0
            ;;
        *) die "no option $1" ;;
    esac
done

# The files the PATH line went into, whichever shells are here.
profiles() {
    for file in "$HOME_DIR/.bashrc" "$HOME_DIR/.bash_profile" "$HOME_DIR/.zshrc" "$HOME_DIR/.profile"; do
        if [ -f "$file" ]; then
            printf '%s\n' "$file"
        fi
    done
}

if [ "$UNINSTALL" = 1 ]; then
    rm -f "${BIN_DIR:?}/cruciblecli"
    rm -rf "${LIB_DIR:?}"
    rm -f "${HOME_DIR:?}/.config/fish/conf.d/cruciblecli.fish"
    profiles | while IFS= read -r file; do
        if grep -qF "$MARK" "$file"; then
            kept="$(mktemp)"
            grep -vF "$MARK" "$file" > "$kept" || true
            cat "$kept" > "$file"
            rm -f "${kept:?}"
        fi
    done
    say "cruciblecli is gone. Your models, settings and chats are still there."
    exit 0
fi

case "$(uname -s)-$(uname -m)" in
    Linux-x86_64|Linux-amd64) ASSET="cruciblecli-linux-x86_64.tar.gz" ;;
    Darwin-arm64|Darwin-aarch64) ASSET="cruciblecli-macos-arm64.tar.gz" ;;
    *) die "there is no download for $(uname -s) on $(uname -m) yet" ;;
esac

if [ -n "$VERSION" ]; then
    URL="https://github.com/$REPO/releases/download/$VERSION/$ASSET"
else
    URL="https://github.com/$REPO/releases/latest/download/$ASSET"
fi

fetch() {
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 3 --progress-bar -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -q --show-progress -O "$2" "$1"
    else
        die "this needs curl or wget"
    fi
}

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK:?}"' EXIT
trap 'exit 130' INT TERM

# CRUCIBLE_CLI_ARCHIVE is a local file in place of the download, for testing.
if [ -n "${CRUCIBLE_CLI_ARCHIVE:-}" ]; then
    cp "$CRUCIBLE_CLI_ARCHIVE" "$WORK/$ASSET"
    if [ -f "$CRUCIBLE_CLI_ARCHIVE.sha256" ]; then
        cp "$CRUCIBLE_CLI_ARCHIVE.sha256" "$WORK/$ASSET.sha256"
    fi
else
    say "Downloading Crucible CLI"
    fetch "$URL" "$WORK/$ASSET" || die "could not download $URL"
    # The checksum is checked when the release has one.
    if ! fetch "$URL.sha256" "$WORK/$ASSET.sha256" 2>/dev/null; then
        rm -f "${WORK:?}/${ASSET:?}.sha256"
    fi
fi

if [ -f "$WORK/$ASSET.sha256" ]; then
    (
        cd "$WORK"
        if command -v sha256sum >/dev/null 2>&1; then
            sha256sum -c "$ASSET.sha256" >/dev/null 2>&1
        elif command -v shasum >/dev/null 2>&1; then
            shasum -a 256 -c "$ASSET.sha256" >/dev/null 2>&1
        fi
    ) || die "the download is damaged. Try again."
fi

tar -xzf "$WORK/$ASSET" -C "$WORK" || die "the download could not be unpacked"
[ -x "$WORK/cruciblecli/bin/cruciblecli" ] || die "the download has no cruciblecli in it"
[ -x "$WORK/cruciblecli/bin/crucible-engine" ] || die "the download has no engine in it"

# Put in place whole. What is running keeps its files until it exits.
mkdir -p "$(dirname "$LIB_DIR")" "$BIN_DIR"
rm -rf "${LIB_DIR:?}.new" "${LIB_DIR:?}.old"
cp -R "$WORK/cruciblecli" "$LIB_DIR.new"
if [ -e "$LIB_DIR" ]; then
    mv "$LIB_DIR" "$LIB_DIR.old"
fi
mv "$LIB_DIR.new" "$LIB_DIR"
rm -rf "${LIB_DIR:?}.old"
ln -sf "$LIB_DIR/bin/cruciblecli" "$BIN_DIR/cruciblecli"

# So that it can be typed in any terminal from now on.
on_path=0
case ":${PATH:-}:" in
    *":$BIN_DIR:"*) on_path=1 ;;
esac
if [ "$on_path" = 0 ]; then
    line="export PATH=\"\$HOME/.local/bin:\$PATH\" $MARK"
    found="$(profiles)"
    if [ -z "$found" ]; then
        printf '%s\n' "$line" > "$HOME_DIR/.profile"
    else
        printf '%s\n' "$found" | while IFS= read -r file; do
            if ! grep -qF "$MARK" "$file"; then
                printf '\n%s\n' "$line" >> "$file"
            fi
        done
    fi
    if command -v fish >/dev/null 2>&1; then
        mkdir -p "$HOME_DIR/.config/fish/conf.d"
        printf 'fish_add_path -g $HOME/.local/bin %s\n' "$MARK" > "$HOME_DIR/.config/fish/conf.d/cruciblecli.fish"
    fi
fi

version="$("$LIB_DIR/bin/cruciblecli" --version 2>/dev/null || true)"
say ""
say "Installed ${version:-cruciblecli}."
if [ "$on_path" = 1 ]; then
    say "Type cruciblecli in any folder."
else
    say "Open a new terminal, then type cruciblecli in any folder."
fi
