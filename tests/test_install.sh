#!/usr/bin/env bash
# Tests for install.sh's pure logic: version comparison, the CUDA-vs-hardware
# table, and CMake version detection. Getting these wrong means a GPU that
# silently cannot be used, so they are worth pinning down.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CRUCIBLE_INSTALL_LIB=1
export CRUCIBLE_INSTALL_LIB
# shellcheck disable=SC1091
source "$HERE/../install.sh"

PASS=0
FAIL=0

check() {
    local what="$1"; shift
    if "$@"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        printf '    FAIL: %s\n' "$what"
    fi
}

check_not() {
    local what="$1"; shift
    if "$@"; then
        FAIL=$((FAIL + 1))
        printf '    FAIL: %s (expected false)\n' "$what"
    else
        PASS=$((PASS + 1))
    fi
}

check_eq() {
    local what="$1" actual="$2" expected="$3"
    if [ "$actual" = "$expected" ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        printf '    FAIL: %s\n         got: %s\n    expected: %s\n' "$what" "$actual" "$expected"
    fi
}

echo "install.sh logic tests"
echo

echo "  cmake_version_ok"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
make_fake_cmake() {
    printf '#!/bin/sh\necho "cmake version %s"\n' "$1" > "$TMP/cmake"
    chmod +x "$TMP/cmake"
}
make_fake_cmake "3.28.3"; check     "3.28.3 accepted" cmake_version_ok "$TMP/cmake"
make_fake_cmake "3.24.0"; check     "3.24.0 accepted" cmake_version_ok "$TMP/cmake"
make_fake_cmake "3.22.1"; check_not "3.22.1 rejected" cmake_version_ok "$TMP/cmake"
make_fake_cmake "4.0.1";  check     "4.0.1 accepted"  cmake_version_ok "$TMP/cmake"
check_not "a missing cmake is rejected" cmake_version_ok "$TMP/definitely-not-here"

# --------------------------------------------------------------------------
# Package lists
#
# Two lists, and only two. A Vulkan SDK and a CUDA toolkit used to be installed
# here as well -- gigabytes of CUDA, on a machine that might never ask for a
# GPU runtime -- so that a runtime could be built later. The program installs
# what a runtime needs when you ask it for one, on the machine that will run
# it, so the installer has no business fetching a compiler for a compile that
# may never happen.
# --------------------------------------------------------------------------
echo
echo "  resolve_packages"

for manager in apt dnf pacman zypper brew; do
    PKG="$manager" resolve_packages
    check_not "$manager names no Vulkan SDK" \
              grep -qiE "vulkan|spirv|glslc|shaderc" <<< "${PKGS_BASE[*]} ${PKGS_GUI[*]}"
    check_not "$manager names no CUDA toolkit" \
              grep -qi "cuda" <<< "${PKGS_BASE[*]} ${PKGS_GUI[*]}"
done

PKG=apt resolve_packages
check     "apt base includes a compiler" \
          grep -q "build-essential" <<< "${PKGS_BASE[*]}"
check     "apt names the desktop app's headers" \
          grep -q "libgl1-mesa-dev" <<< "${PKGS_GUI[*]}"

# resolve_packages ends in a `case`, and a stray non-zero exit there would
# abort the caller under `set -e`. This is the bug that once silently killed
# --check, so it is pinned.
PKG=apt resolve_packages
check_eq  "resolve_packages succeeds" "$?" "0"

# --------------------------------------------------------------------------
# The application-menu entry
#
# A GUI that is only on PATH is not installed as far as a desktop is concerned.
# --------------------------------------------------------------------------
echo
echo "  the desktop entry"

check     "a .desktop file is installed with the GUI" \
          grep -q 'crucible.desktop' "$HERE/../CMakeLists.txt"
check     "and an icon for it" \
          grep -q 'icons/hicolor/scalable/apps' "$HERE/../CMakeLists.txt"
# Both go in the same component, or `--install --component crucible` skips them
# and the uninstaller's manifest never mentions them.
check     "both are in the crucible component" \
          test "$(grep -c 'COMPONENT ${CRUCIBLE_INSTALL_COMPONENT}' "$HERE/../CMakeLists.txt")" -ge 5
# ~/.local/bin is on PATH for a login shell and very often not for a launcher,
# so a bare Exec=crucible-gui is an icon that does nothing when clicked.
check     "Exec is an absolute path, not a bare name" \
          grep -q 'CMAKE_INSTALL_FULL_BINDIR' "$HERE/../CMakeLists.txt"
check     "the entry names the icon" \
          grep -q '^Icon=crucible$' "$HERE/../packaging/crucible.desktop.in"
# Must match the class the window actually sets, or the dock shows the running
# window as a second, generic application.
check     "StartupWMClass matches the window class the app sets" \
          grep -q '^StartupWMClass=crucible-gui$' "$HERE/../packaging/crucible.desktop.in"
check     "and the app really sets that class" \
          grep -q 'GLFW_X11_CLASS_NAME, "crucible-gui"' "$HERE/../src/gui/app.cpp"
# Uninstall has to take them, or the menu keeps offering a Crucible that is gone.
check     "the uninstaller removes the desktop entry" \
          grep -q 'crucible.desktop' "$HERE/../src/app/uninstall.cpp"
check     "and install.sh sweeps it too" \
          grep -q 'share/applications/crucible.desktop' "$HERE/../install.sh"

# --------------------------------------------------------------------------
# pkg_available
#
# This is the check that decides whether the desktop app gets built, so getting
# it wrong costs the whole GUI silently.
#
# It used to be `apt-cache policy "$1" | grep -q ...`. grep -q exits at the
# first match, apt-cache gets a SIGPIPE, and under the `set -o pipefail` this
# script runs with, the pipeline reports 141. Every package on every apt system
# therefore looked unavailable, the desktop app was skipped with "no
# OpenGL/GLFW development packages here", and the install carried on as if that
# were a fact about the machine. These run under pipefail on purpose.
# --------------------------------------------------------------------------
echo
echo "  pkg_available"

if command -v apt-cache >/dev/null 2>&1; then
    set -o pipefail
    # A package that certainly exists wherever apt does. Not a GUI package:
    # this is about the mechanism, not about any one distribution's naming.
    PKG=apt
    check     "an installable package is reported available under pipefail" \
              pkg_available bash
    check_not "a package that does not exist is reported unavailable" \
              pkg_available crucible-no-such-package-exists
    # The failure mode was that *every* name came back false, so a check that
    # only ever asked about one name would have passed throughout.
    real_available=1
    for _p in bash coreutils grep sed; do
        pkg_available "$_p" || real_available=0
    done
    check_eq  "several installable packages in a row all report available" \
              "$real_available" "1"
    set +o pipefail
else
    echo "    (skipped: no apt-cache on this machine)"
fi

# --------------------------------------------------------------------------
# verify_gui_prerequisites
#
# The desktop app is decided by looking for the headers the build actually
# needs, not by trusting a package manager's exit code.
# --------------------------------------------------------------------------
echo
echo "  verify_gui_prerequisites"

WITH_GUI=0
verify_gui_prerequisites
check_eq  "--no-gui is never overridden into a GUI build" "$WITH_GUI" "0"

if [ "$(uname -s)" != "Darwin" ]; then
    if have_header GL/gl.h && { have_header GLFW/glfw3.h || have_header X11/Xlib.h; }; then
        WITH_GUI=1
        verify_gui_prerequisites
        check_eq  "a machine with the headers keeps the desktop app" "$WITH_GUI" "1"
    else
        WITH_GUI=1
        verify_gui_prerequisites
        check_eq  "a machine without the headers steps down to the terminal app" \
                  "$WITH_GUI" "0"
    fi
fi
WITH_GUI=1

# --------------------------------------------------------------------------
# Build directory
#
# Loadable runtimes are shared libraries, which need symlinks. A checkout on
# exFAT cannot hold them, so the build tree has to move.
# --------------------------------------------------------------------------
echo
echo "  choose_build_dir"

SRC_DIR="$TMP/checkout"
mkdir -p "$SRC_DIR"
choose_build_dir
check_eq  "the build tree sits beside the source" "$BUILD_DIR" "$SRC_DIR/build"

# --------------------------------------------------------------------------
# Symlink-free shared libraries
#
# llama.cpp writes libggml-base.so.0.9.4 with libggml-base.so as a symlink
# beside it, and neither exFAT nor NTFS can hold a symlink -- so a checkout on
# an external drive shared with Windows could not be built in. The installer
# used to detect that and move the build tree into the cache directory. It no
# longer has to, because Crucible strips the version suffixes and the libraries
# come out as plain files; these pin that the detection stays gone and the
# stripping stays wired up, in both places that compile llama.cpp.
# --------------------------------------------------------------------------
# Re-running the installer over an old build tree is now the ordinary case,
# not a rare one: the build stays in the checkout instead of being relocated to
# a fresh cache directory. A CMakeCache remembering paths this tree no longer
# has -- a moved home, a restored backup, a source directory pointed elsewhere
# once -- makes cmake refuse to reuse it, and the cache is pure derived data,
# so the installer clears it and tries once more rather than dying on a log.
check     "a stale build cache is cleared and retried, not fatal" \
          grep -q "the existing build directory is stale" "$HERE/../install.sh"
check     "and the retry runs the same configure as the first attempt" \
          test "$(grep -c 'run_configure' "$HERE/../install.sh")" -ge 3

check_not "the installer no longer probes for symlink support" \
          grep -q "symlink-probe" "$HERE/../install.sh"
check_not "and never relocates the build tree" \
          grep -q "no symlinks; building in" "$HERE/../install.sh"
check     "the unversioning helper exists" \
          test -f "$HERE/../cmake/CrucibleUnversion.cmake"
check     "the dependency setup includes it" \
          grep -q "CrucibleUnversion.cmake" "$HERE/../cmake/CrucibleDependencies.cmake"
check     "and sweeps llama.cpp's targets once they exist" \
          grep -q 'crucible_unversion_directory("\${llama_SOURCE_DIR}")' \
               "$HERE/../cmake/CrucibleDependencies.cmake"
check_not "configuring without symlinks is no longer a fatal error" \
          grep -q "CRUCIBLE_FS_HAS_SYMLINKS" "$HERE/../cmake/CrucibleDependencies.cmake"
# The in-app runtime builder runs cmake on llama.cpp directly, with no Crucible
# CMakeLists in the picture, so it has to inject the same thing itself.
check     "the runtime builder injects the same hook" \
          grep -q "CMAKE_PROJECT_INCLUDE" "$HERE/../src/runtime/builder.cpp"
check     "and defines the sweep it points at" \
          grep -q "crucible_unversion_directory" "$HERE/../src/runtime/builder.cpp"

echo
echo "  llama tag"
# A runtime built from a different llama.cpp tag would load and then crash on
# the first tensor. The installer used to carry a copy of the tag so it could
# seed a llama.cpp checkout for the in-app runtime builder; it does not deal in
# runtimes at all now, so there is one place the tag is written down and one
# place it is read.
check_not "the installer does not carry a copy of the llama tag" \
          grep -q "LLAMA_TAG" "$HERE/../install.sh"
check     "CMake pins it" \
          grep -qE 'CRUCIBLE_LLAMA_TAG b[0-9]+' "$HERE/../cmake/CrucibleDependencies.cmake"

# The same tag is compiled into the binary for the in-app runtime builder.
check     "CMakeLists passes the tag to the compiler" \
          grep -q 'CRUCIBLE_LLAMA_TAG="\${CRUCIBLE_LLAMA_TAG}"' "$HERE/../CMakeLists.txt"

# --------------------------------------------------------------------------
# Overall progress
#
# The five parts are nowhere near equal in length, so the main bar weights
# them. A bar that moved a fifth per part would read 80% with the entire build
# still ahead of it, which is worse than showing nothing.
# --------------------------------------------------------------------------
echo
echo "  overall_percent"

check_eq  "weights cover exactly the whole install" \
          "$(( ${STEP_WEIGHTS[1]} + ${STEP_WEIGHTS[2]} + ${STEP_WEIGHTS[3]} \
              + ${STEP_WEIGHTS[4]} + ${STEP_WEIGHTS[5]} ))" "100"
check_eq  "one weight per part, plus the unused zeroth" \
          "${#STEP_WEIGHTS[@]}" "$((STEP_TOTAL + 1))"

# The overall figure is the weighted sum of the finished parts plus however
# far STEP_PCT says the current one has got.
BEFORE_5=$(( ${STEP_WEIGHTS[1]} + ${STEP_WEIGHTS[2]} + ${STEP_WEIGHTS[3]} + ${STEP_WEIGHTS[4]} ))

STEP_PCT=0
STEP_NUM=0; check_eq "nothing done before the first part" "$(overall_percent)" "0"
STEP_NUM=1; check_eq "entering part 1"                    "$(overall_percent)" "0"
STEP_NUM=2; check_eq "entering part 2"                    "$(overall_percent)" "${STEP_WEIGHTS[1]}"
STEP_NUM=5; check_eq "entering part 5"                    "$(overall_percent)" "$BEFORE_5"
STEP_PCT=50
check_eq  "part 5 half done" "$(overall_percent)" "$((BEFORE_5 + STEP_WEIGHTS[5] / 2))"
STEP_PCT=100
check_eq  "part 5 complete"  "$(overall_percent)" "100"

# The build is the part worth weighting for: it must not read as nearly done.
STEP_NUM=5; STEP_PCT=0
check     "the main bar is under half way when the build starts" \
          test "$(overall_percent)" -lt 50

# Past the last part -- what the closing 100% bar sets.
STEP_NUM=$((STEP_TOTAL + 1)); STEP_PCT=0
check_eq  "past the end stays at 100"                     "$(overall_percent)" "100"

# Monotonic: the figure must never go backwards as a part progresses.
STEP_NUM=4
PREV=-1
MONOTONIC=1
for f in 0 10 25 50 75 90 100; do
    STEP_PCT="$f"
    CUR="$(overall_percent)"
    [ "$CUR" -lt "$PREV" ] && MONOTONIC=0
    PREV="$CUR"
done
check_eq  "progress within a part never goes backwards" "$MONOTONIC" "1"
STEP_NUM=0; STEP_PCT=0

# --------------------------------------------------------------------------
# Phases
#
# Each part is divided into phases that own a slice of it, so the per-part bar
# moves through the whole 0-100 whatever the part is doing. The two bars are
# the point of the display: one for the part, one for the install.
# --------------------------------------------------------------------------
echo
echo "  phases"

IS_TTY=0
STEP_NUM=2; STEP_PCT=0

# block_draw reports to stdout when there is no terminal, which is the right
# thing during a real install and only noise here.
phase 40 "first" >/dev/null
check_eq  "a phase starts where the part had got to"  "$STEP_PCT" "0"
phase_at 50 >/dev/null
check_eq  "half of a 40%-wide phase is 20% of the part" "$STEP_PCT" "20"
phase_at 100 >/dev/null
check_eq  "a full phase reaches its own end"           "$STEP_PCT" "40"
phase_end >/dev/null
check_eq  "ending a phase lands on its end exactly"    "$STEP_PCT" "40"

phase 60 "second" >/dev/null
check_eq  "the next phase starts where the last ended" "$PHASE_BASE" "40"
phase_at 50 >/dev/null
check_eq  "and is measured from there"                 "$STEP_PCT" "70"
phase_end >/dev/null
check_eq  "the phases of a part add up to all of it"   "$STEP_PCT" "100"

# cmake restarts its percentage for every target it builds, so a naive
# mapping would send the bar backwards several times during one compile.
STEP_PCT=0
phase 100 "third" >/dev/null
phase_at 90 >/dev/null
phase_at 10 >/dev/null
check_eq  "the bar never retreats"                     "$STEP_PCT" "90"

# A part whose phases add up to more than itself must still stop at 100.
STEP_PCT=0
phase 200 "overrun" >/dev/null
phase_at 100 >/dev/null
check_eq  "a part never reports more than complete"    "$STEP_PCT" "100"

STEP_NUM=0; STEP_PCT=0; PHASE_BASE=0; PHASE_SPAN=0

# --------------------------------------------------------------------------
# Install component
#
# llama.cpp and ggml install themselves into <prefix>/lib unconditionally,
# which for a system-wide install means a libllama.so that can shadow someone
# else's -- and files crucible --uninstall would leave behind. Installing only
# Crucible's own component is what stops that, so the flag is pinned here.
# --------------------------------------------------------------------------
echo
echo "  install component"

check     "CMakeLists puts Crucible's install rules in a component" \
          grep -q "COMPONENT \${CRUCIBLE_INSTALL_COMPONENT}" "$HERE/../CMakeLists.txt"
# Counting install(TARGETS) against COMPONENT used to stand in for this, and
# it is what let the bug through: the counts matched while the component was
# attached to the wrong artifact kind. What has to hold is that *every*
# artifact group in a rule names a component of its own, because options after
# LIBRARY or RUNTIME bind to that kind alone.
every_artifact_group_names_a_component() {
    awk '
        /install\(TARGETS/ { inrule = 1; group = 0; has = 0 }
        inrule {
            if ($0 ~ /(ARCHIVE|LIBRARY|RUNTIME|OBJECTS|FRAMEWORK|BUNDLE)[ \t]+DESTINATION/) {
                if (group && !has) bad = 1
                group = 1; has = 0
            }
            if ($0 ~ /COMPONENT/) has = 1
            if ($0 ~ /\)[ \t]*$/) {
                if (group && !has) bad = 1
                inrule = 0
            }
        }
        END { exit bad ? 1 : 0 }
    ' "$1"
}
check     "every artifact group in an install rule names its component" \
          every_artifact_group_names_a_component "$HERE/../CMakeLists.txt"
check     "the installer asks for that component" \
          grep -q -- "--install .* --component crucible" "$HERE/../install.sh"
check_eq  "every install invocation is scoped" \
          "$(grep -c 'CMAKE" --install' "$HERE/../install.sh")" \
          "$(grep -c -- '--install .* --component crucible' "$HERE/../install.sh")"
# GGML_BACKEND_DIR would bake an absolute search path into the binary and put
# the runtimes outside the component; the path is passed at startup instead.
check_not "GGML_BACKEND_DIR is not set" \
          grep -q "^ *set(GGML_BACKEND_DIR" "$HERE/../cmake/CrucibleDependencies.cmake"

echo
echo "  the bar stays on one row"

# A bar that wraps can never be erased. The redraw moves the cursor up exactly
# one row, so a line that took two leaves the older bar stranded -- and a
# display whose whole point is that there is one of it printed a fresh dead bar
# at every phase, three of them on an ordinary 80-column terminal.
#
# Two things caused it, and both are pinned below. The elapsed clock was not
# counted in the room reserved for the label; and term_cols asked `tput cols`,
# which measures stdout -- and this is called from inside `$( )`, where stdout
# is a pipe, so tput fell back to terminfo's 80 whatever the window was really
# doing. Every window narrower than 80 wrapped every bar.
#
# Measured rather than read: the line is drawn and its columns counted.
bar_line_width() {   # columns, label, seconds on the clock
    ( IS_TTY=1; PROGRESS_ON=1; BLOCK_SHOWN=0; TERM_COLS="$1"
      PHASE_LABEL="$2"; PHASE_START=0; SECONDS="$3"
      STEP_NUM=5; STEP_PCT=50
      block_draw \
        | sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g' | tr -d '\n' | wc -m | tr -d ' ' )
}

# Longer than any terminal here, so fit_label is doing real work, and a clock
# wide enough that forgetting it cannot be hidden by rounding.
BAR_LABEL="fetching https://github.com/mattsaund/Crucible.git into the cache"
for _cols in 30 40 50 60 72 80 100 132; do
    _drawn="$(bar_line_width "$_cols" "$BAR_LABEL" 100000)"
    check "at $_cols columns the bar fits, clock and all ($_drawn)" \
          test "$_drawn" -lt "$_cols"
done
# A short label must not be padded out to the edge either -- a phase with no
# clock yet is the common case and it has to leave the row alone as well.
check "a short label draws a short line" \
      test "$(bar_line_width 80 "configuring" 0)" -lt 80

check     "the room left for the label has the clock taken out of it" \
          grep -qF -- 'fit_label "$PHASE_LABEL" $(( room - ${#clock} ))' "$HERE/../install.sh"
check     "and on a row too tight for both, the clock is what goes" \
          grep -qF -- 'if [ "$room" -lt $(( ${#clock} + 4 )) ]; then' "$HERE/../install.sh"
check     "the width comes from the terminal, not from stdout" \
          grep -qF -- 'stty size </dev/tty' "$HERE/../install.sh"

echo
echo "  bars"
IS_TTY=0
check     "a bar renders its percentage" \
          grep -q "100%" <<< "$(draw_bar 100 "")"
check     "an out-of-range percentage is clamped, not drawn off the end" \
          grep -q "100%" <<< "$(draw_bar 140 "")"
check     "a negative percentage clamps to zero" \
          grep -q "0%" <<< "$(draw_bar -20 "")"

# One bar, for the whole install. There used to be a second bar underneath for
# the part running now; it is gone, and these pin that it stays gone -- two
# progress figures on screen at once is two numbers to reconcile, and the part
# being worked on is already named twice (in the ==> heading and in the label
# beside the bar).
IS_TTY=1
PROGRESS_ON=1
STEP_NUM=3; STEP_PCT=50; PHASE_LABEL="probe"; PHASE_START=0
BLOCK="$(block_draw)"
check     "the bar names the whole install"   grep -q "install" <<< "$BLOCK"
check     "the bar shows the phase label"     grep -q "probe"   <<< "$BLOCK"
check_eq  "the bar is exactly one row"        "$(printf '%s' "$BLOCK" | grep -c '')" "1"
check_not "the bar carries no second percentage" \
          grep -q "%.*%" <<< "$BLOCK"
check_not "the step counter is left to the heading above" \
          grep -q "\[3/5\]" <<< "$BLOCK"

# The elapsed clock. A phase that starts in the very first second has
# PHASE_START=0, and zero is a real start time -- it must not be mistaken for
# the -1 that means "no phase is running".
#
# What is asserted is that a clock appears at all, not what it reads. The
# elapsed figure is SECONDS - PHASE_START, and with PHASE_START=0 that is
# however long this script has been running: pinning it to "(0s)" made the
# check pass or fail on whether the machine was busy enough for the suite to
# take a second to get here.
STEP_NUM=3; STEP_PCT=0; PHASE_LABEL="prompt"
PHASE_START=0
check     "a phase that started in the first second still shows its clock" \
          grep -q "s)" <<< "$(block_draw)"
PHASE_START=-1
check_not "a part with no phase running shows no clock" \
          grep -q "s)" <<< "$(block_draw)"

# The bar goes up before the first part rather than with it, so it is on screen
# from the start of the install instead of appearing a few seconds in.
PROGRESS_ON=1; STEP_NUM=0; STEP_PCT=0; PHASE_LABEL="starting"
check     "the bar is drawn before the first part begins" \
          grep -q "install" <<< "$(block_draw)"

# ...but only once an install is actually under way, which is what keeps it out
# of --check, the banner and the uninstaller. All of those print through note(),
# which draws the block after every line.
PROGRESS_ON=0
check_eq  "nothing is drawn when no install is running" "$(block_draw)" ""

# Retiring it leaves one finished bar in the scrollback and stops the redraws,
# so the closing summary -- which is written with plain printf and knows nothing
# about the cursor arithmetic -- cannot land inside the block.
PROGRESS_ON=1; STEP_NUM=2; STEP_PCT=10; BLOCK_SHOWN=0
# Redirected rather than captured with $(...): a command substitution runs in a
# subshell, so the PROGRESS_ON=0 that retirement depends on would be discarded
# along with it and the test would be checking nothing.
progress_end > "$TMP/retired"
check     "retiring the bar leaves it at 100%" grep -q "100%" "$TMP/retired"
check_eq  "and stops it redrawing"             "$PROGRESS_ON" "0"
check_eq  "and nothing is drawn afterwards"    "$(block_draw)" ""

IS_TTY=0
PROGRESS_ON=0
STEP_NUM=0; STEP_PCT=0

# --------------------------------------------------------------------------
# Runtimes
#
# Crucible installs with no compute backend at all -- not even CPU. The runtimes
# directory is created empty and the settings screen fills it, which is what
# makes the choice of backend reversible.
# --------------------------------------------------------------------------
echo
echo "  the one-line install builds both faces"

# Crucible is two faces over one engine, so the one-line install gives you
# both. The GUI is still the only part that needs anything from the system
# beyond a compiler, which is what the fallback below is for.
check     "the installer builds the desktop app by default" \
          grep -q '^WITH_GUI=1$' "$HERE/../install.sh"
check     "--no-gui opts out" \
          grep -q -- '--no-gui)    WITH_GUI=0' "$HERE/../install.sh"
check     "--gui is still accepted, and still means yes" \
          grep -q -- '--gui)       WITH_GUI=1' "$HERE/../install.sh"
check     "the installer passes the choice through to cmake" \
          grep -q 'DCRUCIBLE_BUILD_GUI=' "$HERE/../install.sh"

# The CMake option stays off. The installers can add the OpenGL and X11 headers
# first and step down to the terminal program when they cannot; a bare `cmake`
# run can do neither, so defaulting it on there would turn a missing system
# header into a failed build.
check     "the CMake option itself stays off for a bare build" \
          grep -q 'option(CRUCIBLE_BUILD_GUI .* OFF)' "$HERE/../CMakeLists.txt"

check     "the GUI's packages are behind the flag" \
          grep -q 'WITH_GUI" = "1" \] && \[ "${#PKGS_GUI\[@\]}"' "$HERE/../install.sh"
# This is what makes the new default safe: a machine with no window library
# gets the terminal program and a warning, not a failed install.
check     "a missing window library falls back to the terminal app" \
          grep -q 'building the terminal program only' "$HERE/../install.sh"
check     "the fallback leaves WITH_GUI off, so the summary tells the truth" \
          grep -q 'WITH_GUI=0$' "$HERE/../install.sh"

# The desktop app was being dropped whenever any one of its seven package names
# was not installable, so a single renamed or retired package cost the whole
# GUI. The set is filtered to what the release carries and the decision is made
# from the headers instead.
check     "one unavailable package no longer cancels the desktop app" \
          grep -q 'gui_have+=(' "$HERE/../install.sh"
check     "the decision is made by looking for the headers" \
          grep -q 'have_header GL/gl.h' "$HERE/../install.sh"
# find_package(OpenGL REQUIRED) and a GLFW that is compiled from source when the
# system has none: those are the two things the build genuinely needs.
check     "GLFW's own headers count, so a system GLFW is enough" \
          grep -q 'have_header GLFW/glfw3.h' "$HERE/../install.sh"
check     "the header check runs even under --no-deps" \
          grep -q '^    verify_gui_prerequisites$' "$HERE/../install.sh"
# The summary used to print a path to crucible-gui whether or not it was there.
check     "the install verifies the desktop binary actually landed" \
          grep -qF '[ ! -x "$PREFIX/bin/crucible-gui" ]' "$HERE/../install.sh"
check     "and says so when the desktop app was not installed" \
          grep -q "crucible-gui  %snot installed" "$HERE/../install.sh"

# Windows: switches default to false, so the opt-out is the switch and the
# build flag has to be derived from it rather than read directly.
check     "Windows takes -NoGui" \
          grep -q '\[switch\] \$NoGui' "$HERE/../install.ps1"
check     "Windows derives the build flag from it" \
          grep -q 'BuildGui = -not \$NoGui' "$HERE/../install.ps1"
check_not "Windows no longer gates the build on the bare -Gui switch" \
          grep -q 'CRUCIBLE_BUILD_GUI="\$(if (\$Gui)' "$HERE/../install.ps1"

echo
echo "  no runtimes are installed"

check_not "the installer builds no runtime" \
          grep -q "prebuild_runtime" "$HERE/../install.sh"
check_not "no backend modules are installed by CMake" \
          grep -q "GGML_AVAILABLE_BACKENDS" "$HERE/../CMakeLists.txt"
check     "the CPU backend is not compiled into the build" \
          grep -q "set(GGML_CPU              OFF CACHE INTERNAL" \
          "$HERE/../cmake/CrucibleDependencies.cmake"
# ggml sets GGML_METAL_DEFAULT and GGML_BLAS_DEFAULT to ON under APPLE, so
# "no backend at all" is only true on a Mac if all three are named explicitly.
for opt in GGML_METAL GGML_BLAS GGML_ACCELERATE; do
    check "the base build turns $opt off (ggml defaults it on for Apple)" \
          grep -q "set($opt *OFF CACHE INTERNAL" \
          "$HERE/../cmake/CrucibleDependencies.cmake"
done

# Same defaults, same problem, one layer down: a CPU runtime built on a Mac
# would otherwise emit ggml-metal and ggml-blas alongside it.
# The desktop app's typeface is compiled in, not looked for. A missing font must
# downgrade to a system face rather than fail the build.
check     "the interface font is fetched and pinned" \
          grep -q 'CRUCIBLE_FONT_TAG' "$HERE/../cmake/CrucibleDependencies.cmake"
check     "a font that will not fetch does not break the build" \
          grep -q 'CRUCIBLE_FONTS_EMBEDDED OFF' \
          "$HERE/../cmake/CrucibleDependencies.cmake"
check     "the app knows whether it has one" \
          grep -q 'CRUCIBLE_HAS_EMBEDDED_FONT' \
          "$HERE/../cmake/CrucibleDependencies.cmake"

echo
echo "  Windows"

# The Windows installer is a separate script, because PowerShell is what is
# there and bash is not. It has to offer the same shape of thing.
check     "there is a Windows installer" \
          test -f "$HERE/../install.ps1"
check     "it takes the same options the shell one does" \
          grep -q 'param(' "$HERE/../install.ps1"
for opt in Prefix Gui Uninstall Check; do
    check "  -$opt" grep -q "\\\$$opt" "$HERE/../install.ps1"
done
check_not "and no longer takes a GPU SDK to install" \
          grep -q -- '-Gpu\|\$Gpu' "$HERE/../install.ps1"
# It must not build a runtime either: that decision is the same on every
# platform, and it belongs to the settings screen.
check_not "it does not build a GPU runtime" \
          grep -q 'CRUCIBLE_BUILD_RUNTIME\|prebuild_runtime' "$HERE/../install.ps1"
check     "it installs only Crucible's own component" \
          grep -q -- '--component crucible' "$HERE/../install.ps1"
# RPATH is an ELF idea. On Windows the loader looks beside the executable, so
# the libraries have to be installed there instead.
check     "Windows installs the libraries beside the binary" \
          grep -q 'RUNTIME DESTINATION \${CMAKE_INSTALL_BINDIR}' "$HERE/../CMakeLists.txt"
check     "and does not try to bake in an RPATH" \
          grep -q 'CRUCIBLE_BACKEND_DL AND NOT WIN32' "$HERE/../CMakeLists.txt"
# MSVC assumes the system code page without this and mangles every non-ASCII
# glyph the sprite and the expert panel draw with.
check     "MSVC is told the sources are UTF-8" \
          grep -q 'add_compile_options(/utf-8)' "$HERE/../CMakeLists.txt"

echo
echo "  the built-in fallback seat is gone"

# It was a tenth expert the delegator could never name. Any ordinary seat can
# play that part now, and routing.default_expert says which.
check_not "no expert is special-cased as a fallback" \
          grep -rq 'kFallbackId' "$HERE/../src" "$HERE/../include"
check_not "no seat is marked unroutable" \
          grep -rq 'routable' "$HERE/../src" "$HERE/../include"
check     "the nominated default is what catches the rest" \
          grep -q 'default_expert' "$HERE/../include/crucible/config/config.hpp"

echo
echo "  a cook can change hands"

# The verb table specifically, not the instructions -- those name it too, so a
# grep for the word alone would pass with the verb renamed out from under it.
check     "HANDOFF is in the verb table" \
          grep -q '"HANDOFF", ToolKind::Handoff' "$HERE/../src/tools/workshop.cpp"
check     "the cook loop re-routes on one" \
          grep -q 'take_the_seat' "$HERE/../src/engine/engine_cook.cpp"
# The whole memory argument for the design: one expert resident at a time.
check     "the previous expert is freed before the next is loaded" \
          grep -q 'acquire_expert' "$HERE/../src/engine/engine_cook.cpp"

echo
echo "  no runtimes are installed (continued)"

check     "a runtime build turns off BLAS and Accelerate" \
          grep -q -- '-DGGML_BLAS=OFF' "$HERE/../src/runtime/builder.cpp"
check     "a runtime build turns off Metal unless Metal is what was asked for" \
          grep -q "kind != BackendKind::Metal" "$HERE/../src/runtime/builder.cpp"

# The installer used to copy the llama.cpp checkout the build had already
# fetched into the data directory, so that building a runtime later needed no
# network. It was still runtime work in an installer that does not install
# runtimes, and it left several hundred megabytes on disk for a compile that
# might never be asked for. The program fetches its own source when you ask it
# for a runtime.
check_not "the installer seeds no llama.cpp checkout" \
          grep -q "seed_runtime_source\|runtime-src" "$HERE/../install.sh"
check_not "and its summary no longer reports a runtime that was never installed" \
          grep -q "runtimes :" "$HERE/../install.sh"
check_not "nor a \"next steps\" list telling you to go and build one" \
          grep -q "open .*Runtimes.*Pick CPU" "$HERE/../install.sh"

echo "  failure reporting"

# The report Matt got from the Mac was 25 lines of successful status messages
# and no error, because the tail of a log is not where the error necessarily
# is. A failure report that omits the failure costs a whole round trip.
buried_log="$(mktemp)"; buried_out="$(mktemp)"
{
    echo "CMake Error at ggml/src/CMakeLists.txt:592 (add_subdirectory):"
    echo "  The source directory does not contain a CMakeLists.txt file."
    i=1
    while [ "$i" -le 30 ]; do echo "-- harmless status line $i"; i=$((i + 1)); done
} > "$buried_log"
show_log_tail "$buried_log" 25 > "$buried_out" 2>&1 || true

check     "an error buried above the tail is still reported" \
          grep -q "CMake Error" "$buried_out"
check     "the tail is shown as well, not instead" \
          grep -q "harmless status line 30" "$buried_out"
rm -f "$buried_log" "$buried_out"

echo "  bash 3.2 on macOS"

# macOS ships bash 3.2.57 and always will -- Apple froze it in 2007 over the
# GPLv3 -- so `#!/usr/bin/env bash` finds 3.2 on a machine that has not
# installed a newer one, which is every machine this script is run on.
#
# 3.2 scans a bare $name with isalnum() a byte at a time, and macOS's ctype
# table answers yes for the bytes of a UTF-8 character. So a bare colour
# variable written against a tick becomes a variable whose name has the tick's
# three bytes glued on the end, which is unset, and `set -u` kills the install --
# which is exactly what happened at step 2/5, reported as
# "C_GRN?: unbound variable" because the terminal cannot print the bytes back.
#
# Linux never shows this: glibc's isalnum says no for those bytes, so the same
# line is correct here and fatal there. Braces are the fix, and this is the
# check that keeps them.
# grep -P is GNU-only, and this check has to run on the machine it is about.
# LC_ALL=C makes the bracket a byte range rather than a character one, which is
# the whole point: it is the raw bytes 3.2 misreads.
check_not "no bare \$VAR is followed by a non-ASCII character (bash 3.2 eats it)" \
          env LC_ALL=C grep -qE '[$][A-Za-z_][A-Za-z0-9_]*[^ -~'"$(printf '\t')"']' \
          "$HERE/../install.sh" "$HERE/../tests/test_install.sh" \
          "$HERE/../tests/test_uninstall.sh"

# The 4.x features that would fail the same way: silently on Linux, fatally on
# a Mac. Associative arrays and the upper/lower-casing expansions are the ones
# most likely to be reached for here.
check_not "no bash 4+ syntax the Mac's shell cannot parse" \
          grep -qE '(declare|local) -[A]|(declare|local) -[n]|map[f]ile|read[a]rray|[$][{][A-Za-z_][A-Za-z0-9_]*(\^\^|,,)' \
          "$HERE/../install.sh" "$HERE/../tests/test_install.sh" \
          "$HERE/../tests/test_uninstall.sh"

# An empty array expanded as "${arr[@]}" is an unbound variable in 3.2 -- so
# the Mac, where PKGS_GUI is empty, is the one machine where it fires.
# ${arr[@]+"${arr[@]}"} is the portable spelling.
check_not "no bare \"\${arr[@]}\" expansion (empty arrays are fatal in bash 3.2)" \
          grep -qE '[^+]"[$][{]PKGS_[A-Z]*\[@\][}]"' "$HERE/../install.sh"

echo
echo "  the crucible component carries the libraries the binary links against"

# The bug this pins down: in install(TARGETS), every option after LIBRARY or
# RUNTIME binds to that artifact kind alone. A single trailing COMPONENT
# attaches to RUNTIME and leaves the LIBRARY rule in CMake's default
# "Unspecified" component, which `--install --component crucible` never
# installs. The binaries land, the shared objects they need do not, and
# crucible dies on startup with
#   "libllama.so: cannot open shared object file: No such file or directory".
#
# Read off the generated install script rather than by installing: it names the
# component of every file, needs no build, and is what CMake will actually run.
filed_under_crucible() {
    awk -v want="$2" '
        /^if\(CMAKE_INSTALL_COMPONENT STREQUAL "/ {
            component = $0
            sub(/.*STREQUAL "/, "", component)
            sub(/".*/, "", component)
        }
        index($0, want) { seen = 1; if (component != "crucible") wrong = 1 }
        END { exit (seen && !wrong) ? 0 : 1 }
    ' "$1"
}

GENERATED="$HERE/../build/cmake_install.cmake"
if [ -f "$GENERATED" ]; then
    for _lib in libllama.so libggml.so libggml-base.so; do
        check "$_lib is filed under the crucible component" \
              filed_under_crucible "$GENERATED" "lib/crucible/$_lib"
    done
    check "the crucible binary is filed under the crucible component" \
          filed_under_crucible "$GENERATED" "bin/crucible"
else
    echo "    (skipped: no configured build tree at build/)"
fi

echo
echo "  the installer installs the program, and only the program"

ROOT="$HERE/.."

# It used to detect the GPU, pick a backend, install a Vulkan SDK on every
# machine and offer several gigabytes of CUDA on some -- all so that a runtime
# could be compiled later, from the settings screen, by the program. None of
# that is an installer's work: which backend to build is a question that can
# only be answered on the machine at the moment it is asked, and the program
# asks it there. What is left is a compile and a copy.
for _gone in decide_backend cuda_required_for_cap nvcc_version apt_cuda_candidate \
             seed_runtime_source CUDA_NOTE PKGS_VULKAN PKGS_CUDA RUNTIME BACKEND_LIST; do
    check_not "install.sh no longer carries $_gone" \
              grep -q "$_gone" "$ROOT/install.sh"
done
check_not "and takes no --gpu option to argue about" \
          grep -q -- "--gpu" "$ROOT/install.sh"
check_not "it does not go looking for a graphics card" \
          grep -q "nvidia-smi" "$ROOT/install.sh"
check_not "the Windows installer does not either" \
          grep -q "Win32_VideoController\|Resolve-Runtime" "$ROOT/install.ps1"

# The program's own SDK advice is what replaced all of it: refuse before the
# build rather than fail inside it, and name the command for the package
# manager this machine actually has.
check     "the program names the tool each backend needs" \
          grep -q "required_tool" "$ROOT/src/runtime/backend.cpp"
check     "and the command that would install it" \
          grep -q "install_hint" "$ROOT/include/crucible/runtime/backend.hpp"

# What it does install: the program, and the directories the program will use.
check     "the installer creates the directories the program keeps files in" \
          grep -q "^make_directories() {" "$ROOT/install.sh"
for _dir in CONFIG_DIR DATA_DIR MODELS_DIR; do
    check "  $_dir" grep -qF -- "\"\$$_dir\"" "$ROOT/install.sh"
done
check     "and the Windows one creates the same three" \
          grep -q 'New-Item .*-Path \$ConfigDir, \$DataDir, \$ModelsDir' "$ROOT/install.ps1"

echo
echo "  one progress bar, and nothing else"

# The bar used to have five step headings scrolling above it and a running
# commentary under each -- what was detected, what was installed, what every
# phase achieved. All of it was already said by the label beside the bar, in
# the moment it was true.
check_not "a part no longer announces itself with a heading" \
          grep -q '==>%s %s\[%d/%d\]' "$ROOT/install.sh"
check     "the running commentary is silent while the bar is up" \
          grep -q 'info()  { if \[ "$PROGRESS_ON" != 1 \]' "$ROOT/install.sh"
check     "so is the per-phase 'done' line" \
          grep -q 'ok()    { if \[ "$PROGRESS_ON" != 1 \]' "$ROOT/install.sh"
# A warning is exempt: it says something is not as asked -- a desktop app that
# cannot be built here -- and that has to reach the screen whatever is on it.
check     "a warning still gets through" \
          grep -q 'warn()  { note ' "$ROOT/install.sh"
check     "Windows draws one bar too" \
          grep -q "function Show-Bar" "$ROOT/install.ps1"
# Same trap, same fix: a bar padded to a width the window does not have wraps,
# and a wrapped bar can never be rewritten in place.
check     "and measures the console rather than assuming a width" \
          grep -q 'RawUI.WindowSize.Width' "$ROOT/install.ps1"
check_not "with no hard-coded row width left to be wrong about" \
          grep -qE "PadRight\(78\)|' \* 78" "$ROOT/install.ps1"
check     "and keeps its notes quiet while it is up" \
          grep -q 'if (-not \$script:BarShown) { Write-Host "    \$Message"' "$ROOT/install.ps1"

# The closing text. No "Next:" list -- it told you to go and build a runtime,
# put models somewhere and assign them, which is the program's first-run job
# and not something to read once at the end of an install and then scroll past.
check_not "no next-steps list at the end of the install" \
          grep -q "Next:" "$ROOT/install.sh"
check_not "and no note about a GPU backend it did not install" \
          grep -q "About the GPU backend" "$ROOT/install.sh"

echo
echo "  install and uninstall are each one line"

# Symmetry is the point: whatever you typed to put it there, the same command
# with --uninstall takes it away -- including from a machine where the binary
# is too broken to uninstall itself.
check     "the installer documents its own uninstall one-liner" \
          grep -qF -- 'bash -s -- --uninstall' "$ROOT/install.sh"
check     "and prints it when the install finishes" \
          grep -qF -- 'To remove it:' "$ROOT/install.sh"
check     "the README gives the same two lines" \
          grep -qF -- 'bash -s -- --uninstall' "$ROOT/README.md"
check     "including the PowerShell spelling, which iex cannot express alone" \
          grep -qF -- 'scriptblock]::Create((irm' "$ROOT/README.md"
check     "and the Windows installer prints it too" \
          grep -qF -- 'scriptblock]::Create((irm' "$ROOT/install.ps1"

echo
echo "  the tagline"

# One sentence, in four places, and they have to agree.
TAGLINE="a local LLM engine that delegates."
for _where in install.sh install.ps1 src/app/cli.cpp README.md; do
    check "$_where carries the tagline" \
          grep -qF -- "$TAGLINE" "$ROOT/$_where"
done
check_not "and nowhere still says the old one" \
          grep -rqF -- "a local forge" "$ROOT/install.sh" "$ROOT/install.ps1" \
               "$ROOT/src" "$ROOT/README.md"

echo
echo "  one mark, and it is a flame"

# The program used to be drawn as a crucible: a pot on a stand with a fire under
# it, copied by hand into the banner, both installers, the README and the
# terminal sprite. Five copies of one picture, which is four too many to keep in
# step -- and the desktop window and the launcher icon had already moved on to
# the flame.
#
# These pin the removal rather than the drawing. The vessel is gone from every
# file that carried it, and every place that shows a mark shows the same one.
ROOT="$HERE/.."

# The pot's rim and its stand, and the flame that was drawn out of slashes and
# parentheses after it. Distinctive enough that no ordinary line of prose, shell
# or C++ contains them, and the pieces most likely to be left behind by a
# half-finished edit.
# This file is excluded because it is the one place they are still written down:
# the patterns above are what it is looking for.
for _art in ",-----------," "'-----------'" "\\_______/" "(/^\\)" \
            "( (  ) )" "\\__/" "\\ \\/ /" "(  /\\  )"; do
    check_not "no file still draws \"$_art\"" \
              grep -rqF --exclude=test_install.sh -- "$_art" \
                   "$ROOT/src" "$ROOT/include" "$ROOT/tests" \
                   "$ROOT/README.md" "$ROOT/install.sh" "$ROOT/install.ps1"
done

# The still mark is packaging/flame.txt, pasted into four places. Checked by a
# row of the drawing rather than by the whole thing: a row is enough to tell a
# stale copy from a current one, and the whole art in a test file is a fifth
# copy to keep in step.
FLAME_ROW="$(sed -n '7p' "$ROOT/packaging/flame.txt")"
check_not "the mark exists to be pasted from" [ -z "$FLAME_ROW" ]
for _where in install.sh install.ps1 src/app/cli.cpp README.md; do
    check "$_where carries the flame from packaging/flame.txt" \
          grep -qF -- "$FLAME_ROW" "$ROOT/$_where"
done

# The terminal sprite is the fifth place, and the one that cannot paste: it has
# to animate, and it has to sit in the nine columns a roster of seats leaves
# beside it. So it carries the same drawing resampled rather than the rows
# above -- braille either way, and a hash-filled silhouette for the terminal
# whose font has no braille in it.
check     "the terminal sprite is drawn in braille too" \
          grep -q "⣿" "$ROOT/src/ui/widgets/flame_sprite.cpp"
check     "and falls back to hashes rather than to boxes" \
          grep -qF -- "#########" "$ROOT/src/ui/widgets/flame_sprite.cpp"
check_not "it is not the full-size drawing pasted in, which would not fit" \
          grep -qF -- "$FLAME_ROW" "$ROOT/src/ui/widgets/flame_sprite.cpp"

# Braille is not ASCII, so both faces have to put the console into UTF-8 before
# they print it. Without this the Windows console renders every byte of every
# glyph as its own question mark, which is the first thing a Windows user sees.
check     "the terminal binary sets the console to UTF-8 before printing" \
          grep -q "use_utf8_console" "$ROOT/src/main.cpp"
check     "so does the desktop one" \
          grep -q "use_utf8_console" "$ROOT/src/gui/main.cpp"
check     "and the Windows installer does the same for itself" \
          grep -q "OutputEncoding" "$ROOT/install.ps1"

# The window and the launcher icon are the same shape from the same numbers, so
# a reshape that touches one and not the other is the failure to catch.
check     "the icon points at the source of the shape" \
          grep -q "flame_path" "$ROOT/packaging/crucible.svg"
check     "and the shape says the icon is drawn from it" \
          grep -q "packaging/crucible.svg" "$ROOT/src/gui/theme.cpp"

echo
echo "$((PASS + FAIL)) checks, $FAIL failed"
[ "$FAIL" -eq 0 ]
