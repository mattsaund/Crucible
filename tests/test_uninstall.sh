#!/usr/bin/env bash
# Safety tests for `crucible --uninstall`.
#
# Uninstall is the only destructive thing Crucible does, and a clean reinstall
# test depends on it actually being clean: one yes must leave nothing behind,
# and one no must leave everything. Pinned down by running the real binary
# against a sandboxed HOME rather than by reading the code.
#
# There used to be three questions -- programs, then config, then models and
# data -- and a test here for each combination. One question now takes
# everything, so what is pinned is that it really does take everything, and
# that declining takes nothing.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CRUCIBLE="${1:-$HERE/../build/bin/crucible}"

if [ ! -x "$CRUCIBLE" ]; then
    echo "crucible binary not found at $CRUCIBLE -- build first" >&2
    exit 2
fi

# These tests run the real binary, so a stale one reports failures that look
# like product bugs and are not. Refuse rather than mislead.
#
# src/gui is excluded because nothing in it is linked into `crucible` -- it is
# the desktop application, built from the same core into a different binary.
# Editing it left this refusing to run against a `crucible` that was in fact
# perfectly current.
#
# The parentheses matter: without them find reads this as
# "(-name '*.cpp') OR ('*.hpp' AND -newer)", which matches every source file
# whatever its age.
NEWER="$(find "$HERE/../src" "$HERE/../include" \
         -path "$HERE/../src/gui" -prune -o \
         \( -name '*.cpp' -o -name '*.hpp' \) -newer "$CRUCIBLE" -print 2>/dev/null | head -1)"
if [ -n "$NEWER" ]; then
    echo "$CRUCIBLE is older than $NEWER -- rebuild, or pass the right binary" >&2
    exit 2
fi

PASS=0
FAIL=0
check() {
    local what="$1" got="$2" want="$3"
    if [ "$got" = "$want" ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        printf '    FAIL: %s\n         got: %s\n    expected: %s\n' "$what" "$got" "$want"
    fi
}

SANDBOX="$(mktemp -d)"
trap 'rm -rf "$SANDBOX"' EXIT

# Build a throwaway install: a copy of the binary, a config, and two "models".
setup() {
    local root="$SANDBOX/$1"
    rm -rf "$root"
    mkdir -p "$root/bin" "$root/cfg/crucible" "$root/dat/crucible/models"
    # The bootstrapped CMake and, on a filesystem without symlinks, the whole
    # build tree live in the cache. Uninstall removes it -- which is exactly
    # why this has to be sandboxed: without XDG_CACHE_HOME below, running
    # these tests deletes the developer's own build directory.
    mkdir -p "$root/cache/crucible/build"
    head -c 65536 /dev/zero > "$root/cache/crucible/build/CMakeCache.txt"
    # The installed layout: the binary in bin/ and llama.cpp's shared
    # libraries in lib/crucible/. The binary is not the whole program any more,
    # and uninstall has to know that.
    #
    # These have to be the real libraries, not stand-ins. A loadable build
    # resolves libllama.so through an RPATH of $ORIGIN/../lib/crucible, so a
    # sandbox holding a zero-filled file of that name gives a binary that
    # cannot start -- and every check then reads "nothing was removed", which
    # looks exactly like an uninstaller that does nothing.
    mkdir -p "$root/lib/crucible/runtimes"
    local built=0
    for lib in "$(dirname "$CRUCIBLE")"/lib*.so; do
        [ -e "$lib" ] || continue
        cp "$lib" "$root/lib/crucible/"
        built=1
    done
    if [ "$built" = 0 ]; then
        # A monolithic build has no shared libraries at all; the file only has
        # to exist for the "libraries removed" check to mean something.
        head -c 262144 /dev/zero > "$root/lib/crucible/libllama.so"
    fi
    head -c 262144 /dev/zero > "$root/lib/crucible/runtimes/libggml-cpu-haswell.so"
    # A GPU runtime the user built, plus the source and build tree behind it.
    mkdir -p "$root/dat/crucible/runtimes" "$root/dat/crucible/runtime-src" \
             "$root/dat/crucible/runtime-build/vulkan" "$root/dat/crucible/projects/demo-abc12345"
    head -c 524288 /dev/zero > "$root/dat/crucible/runtimes/libggml-vulkan.so"
    echo '{"vulkan":{"llama_tag":"b10678"}}' > "$root/dat/crucible/runtimes/manifest.json"
    echo '{"turns":[]}' > "$root/dat/crucible/projects/demo-abc12345/usage.json"
    cp "$CRUCIBLE" "$root/bin/crucible"
    # The installer puts these beside the binary, so uninstall has to take them.
    # crucible-gui is the one that was missed: the desktop app is installed by
    # the same component and was being left on PATH, pointing at libraries the
    # uninstaller had just deleted.
    printf '#!/bin/sh\n' > "$root/bin/crucible-routebench"; chmod +x "$root/bin/crucible-routebench"
    printf '#!/bin/sh\n' > "$root/bin/crucible-gui";        chmod +x "$root/bin/crucible-gui"
    echo '{"models_dir":""}' > "$root/cfg/crucible/config.json"
    echo '{"trusted":[]}'    > "$root/cfg/crucible/trust.json"
    head -c 1048576 /dev/zero > "$root/dat/crucible/models/expensive-expert.gguf"
    head -c 1048576 /dev/zero > "$root/dat/crucible/models/another-expert.gguf"
    printf '%s' "$root"
}

run_uninstall() {  # root, answers, extra args
    local root="$1" answers="$2"; shift 2
    printf '%b' "$answers" | XDG_CONFIG_HOME="$root/cfg" XDG_DATA_HOME="$root/dat" \
        XDG_CACHE_HOME="$root/cache" \
        "$root/bin/crucible" --uninstall "$@" >/dev/null 2>&1
}

count_models() { ls "$1/dat/crucible/models" 2>/dev/null | wc -l | tr -d ' '; }
exists()       { [ -e "$1" ] && echo yes || echo no; }

echo "crucible --uninstall safety tests"
echo

echo "  declining leaves everything in place"
ROOT="$(setup declining)"
run_uninstall "$ROOT" 'n\n'
check "binary kept"           "$(exists "$ROOT/bin/crucible")"            "yes"
check "desktop app kept"      "$(exists "$ROOT/bin/crucible-gui")"         "yes"
check "config kept"           "$(exists "$ROOT/cfg/crucible/config.json")" "yes"
check "models kept"           "$(count_models "$ROOT")"                  "2"

echo "  one yes leaves nothing behind"
# What a clean reinstall test needs: one pass, one answer, nothing left to tidy
# up by hand.
ROOT="$(setup everything)"
run_uninstall "$ROOT" 'y\n'
check "binary removed"        "$(exists "$ROOT/bin/crucible")"            "no"
check "config removed"        "$(exists "$ROOT/cfg/crucible")"            "no"
check "data removed"          "$(exists "$ROOT/dat/crucible")"            "no"
# The shared libraries are most of the install by size; leaving them behind
# would make "yes to everything" a lie.
check "libraries removed"     "$(exists "$ROOT/lib/crucible")"            "no"
check "routebench removed"    "$(exists "$ROOT/bin/crucible-routebench")"  "no"
check "desktop app removed"  "$(exists "$ROOT/bin/crucible-gui")"         "no"
check "user runtimes removed" "$(exists "$ROOT/dat/crucible/runtimes")"   "no"
check "runtime source removed" "$(exists "$ROOT/dat/crucible/runtime-src")" "no"
check "project history removed" "$(exists "$ROOT/dat/crucible/projects")" "no"
check "cache removed"         "$(exists "$ROOT/cache/crucible")"          "no"

echo "  -y answers the question"
ROOT="$(setup assume_yes)"
run_uninstall "$ROOT" '' -y
check "binary removed"        "$(exists "$ROOT/bin/crucible")"            "no"
check "config removed"        "$(exists "$ROOT/cfg/crucible")"            "no"
check "data removed"          "$(exists "$ROOT/dat/crucible")"            "no"
# The shared libraries are most of the install by size; leaving them behind
# would make "yes to everything" a lie.
check "libraries removed"     "$(exists "$ROOT/lib/crucible")"            "no"
check "routebench removed"    "$(exists "$ROOT/bin/crucible-routebench")"  "no"
check "desktop app removed"  "$(exists "$ROOT/bin/crucible-gui")"         "no"
check "user runtimes removed" "$(exists "$ROOT/dat/crucible/runtimes")"   "no"
check "runtime source removed" "$(exists "$ROOT/dat/crucible/runtime-src")" "no"
check "project history removed" "$(exists "$ROOT/dat/crucible/projects")" "no"
check "cache removed"         "$(exists "$ROOT/cache/crucible")"          "no"

echo "  there is no half-uninstall to land in by accident"
# The old three questions made "programs gone, config and models left" the
# result of two keystrokes, and it is the one outcome nobody wants: a machine
# with no Crucible on it and Crucible's files all over it. One answer decides
# the lot, so a single "yes" has to reach the config and the models too.
ROOT="$(setup no_partial)"
run_uninstall "$ROOT" 'y\n'
check "config removed"        "$(exists "$ROOT/cfg/crucible")"            "no"
check "models removed"        "$(count_models "$ROOT")"                  "0"
check "nothing is left in the prefix" \
      "$(ls "$ROOT/bin" "$ROOT/lib" 2>/dev/null | grep -c crucible)"     "0"

echo "  and no half-answer either: one no keeps the models"
ROOT="$(setup keep_everything)"
run_uninstall "$ROOT" 'n\n'
check "binary kept"           "$(exists "$ROOT/bin/crucible")"            "yes"
check "models kept"           "$(count_models "$ROOT")"                  "2"

echo "  a prompt that cannot be read is not an answer of no"
# The documented one-liner is `curl ... | bash -s -- --uninstall`, and install.sh
# delegates to this binary. stdin there is the installer script still being
# read, so the questions were answered with lines of shell -- every one taken as
# "no" -- and the command printed "Done." having removed nothing. The exit code
# was 0, so nothing downstream could tell.
ROOT="$(setup unreadable_stdin)"
# Stand in for the curl pipe: text that is not an answer, then end of input.
head -c 400 "$HERE/../install.sh" | XDG_CONFIG_HOME="$ROOT/cfg" \
    XDG_DATA_HOME="$ROOT/dat" XDG_CACHE_HOME="$ROOT/cache" \
    "$ROOT/bin/crucible" --uninstall >/dev/null 2>&1
check "an unreadable prompt fails rather than reporting success" "$?" "1"

# And with a terminal -- which is what install.sh now hands it -- the same
# install comes apart completely. This is the one-liner's real path.
ROOT="$(setup delegated)"
run_uninstall "$ROOT" 'y\n'
check "binary removed"        "$(exists "$ROOT/bin/crucible")"            "no"
check "desktop app removed"   "$(exists "$ROOT/bin/crucible-gui")"        "no"
check "config removed"        "$(exists "$ROOT/cfg/crucible")"            "no"
check "data removed"          "$(exists "$ROOT/dat/crucible")"            "no"

echo "  install.sh --uninstall gives the binary a terminal to ask on"
# The fix is in the delegation, not in the binary: install.sh reads its own
# questions from /dev/tty for exactly this reason and has to hand the same
# terminal on.
# Fixed strings: these are literal lines of shell, and `$?` in a basic regex
# does not mean what it looks like.
greps() { grep -qF -- "$1" "$HERE/../install.sh" && echo yes || echo no; }
check "the delegation redirects stdin from the terminal" \
      "$(greps '--uninstall < /dev/tty')" "yes"
check "with no terminal at all it answers yes rather than silently nothing" \
      "$(greps '"$exe" --uninstall --yes || status=$?')" "yes"
# The script's own sweep, for a prefix with no binary in it, asks once and
# names everything -- the same bargain the binary offers.
check "the shell sweep asks one question, not three" \
      "$(greps 'Remove Crucible and everything above?')" "yes"
check "and the question covers the config and the data as well" \
      "$(greps 'for entry in "$cfg" "$dat" "$cache"; do')" "yes"
# A machine with both a system and a user install had the second one left
# behind, on PATH, pointing at libraries the first pass had just deleted.
check "every prefix is uninstalled, not just the first" \
      "$(greps 'handled+=("$p")')" "yes"
# `[ -r /dev/tty ]` is true on a process with no controlling terminal -- the
# node is there, and it is the open that fails with ENXIO. Testing readability
# instead of opening it sent the one-liner down the terminal path with no
# terminal, and the uninstall removed nothing.
check "the terminal test opens /dev/tty rather than stat-ing it" \
      "$(greps '( exec </dev/tty ) 2>/dev/null')" "yes"
# A delegation that could not finish has settled nothing, so the shell sweep
# must not read the binary still being there as the user declining.
check "a failed delegation falls back to the sweep" \
      "$(greps 'if [ "$status" -eq 0 ]; then')" "yes"
# And the mirror of it. A delegation that finished and left its own binary
# behind is one where the answer was no -- so the sweep must not then delete
# the whole prefix, which is exactly what was just declined.
check "a declined delegation stops rather than sweeping" \
      "$(greps 'if [ "$status" -eq 0 ] && [ -e "$exe" ]; then')" "yes"
echo "  a bare install with nothing to remove does not fail"
ROOT="$SANDBOX/bare"
mkdir -p "$ROOT/bin" "$ROOT/cfg" "$ROOT/dat"
cp "$CRUCIBLE" "$ROOT/bin/crucible"
# XDG_CACHE_HOME as well, for the same reason as run_uninstall above: the
# uninstaller removes the build cache, and an unsandboxed run of this test
# would remove the real one belonging to whoever is running the suite.
printf 'n\n' | XDG_CONFIG_HOME="$ROOT/cfg" XDG_DATA_HOME="$ROOT/dat" \
    XDG_CACHE_HOME="$ROOT/cache" \
    "$ROOT/bin/crucible" --uninstall >/dev/null 2>&1
check "exit code"             "$?"                                       "0"

echo
echo "$((PASS + FAIL)) checks, $FAIL failed"
[ "$FAIL" -eq 0 ]
