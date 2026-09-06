#!/usr/bin/env bash
#
# Crucible installer.
#
#   curl -fsSL https://raw.githubusercontent.com/mattsaund/Crucible/main/install.sh | bash
#
#   curl -fsSL https://raw.githubusercontent.com/mattsaund/Crucible/main/install.sh | bash -s -- --uninstall
#
# Installs the program: the build toolchain, a compile, `crucible` and
# `crucible-gui` on your PATH, and the directories they keep their files in.
# Nothing else. No compute runtime and no models -- Crucible manages its own
# runtimes from the settings screen, on the machine that will run them, so an
# installer that guessed at a GPU SDK was doing work the program does better
# and asking questions it does not need the answers to.
#
# Safe to re-run: it upgrades in place.

set -euo pipefail

REPO_URL="https://github.com/mattsaund/Crucible.git"
RAW_URL="https://raw.githubusercontent.com/mattsaund/Crucible/main/install.sh"

BRANCH="main"

# The desktop application. Built by default: Crucible is two faces over one
# engine, and someone who runs the one-line install should get both of them.
#
# It is the only part that needs anything from the system beyond a compiler --
# OpenGL and, on Linux, a handful of X11 development headers, a few megabytes
# in total, which this installs. Where those cannot be had the build steps
# down to the terminal program with a warning rather than failing, so turning
# this on cannot cost anyone their install. --no-gui skips it outright.
WITH_GUI=1

PREFIX=""
INSTALL_DEPS=1
ASSUME_YES=0
DO_UNINSTALL=0
DO_CHECK=0
# nproc is GNU coreutils; macOS has sysctl instead. Four is the fallback when
# neither answers, which is a build that is slower than it could be rather than
# one that does not happen.
JOBS="$(command -v nproc >/dev/null 2>&1 && nproc \
        || sysctl -n hw.ncpu 2>/dev/null \
        || echo 4)"

CMAKE_MIN_MAJOR=3
CMAKE_MIN_MINOR=24
CMAKE_BOOTSTRAP_VERSION="3.31.6"

CMAKE="cmake"
SUDO=""
PKG=""
SRC_DIR=""
CLONED_FRESH=0

# --------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    C_RESET=$'\033[0m'; C_BOLD=$'\033[1m'; C_DIM=$'\033[2m'
    C_YEL=$'\033[33m';  C_GRN=$'\033[32m'; C_RED=$'\033[31m'; C_CYN=$'\033[36m'
else
    C_RESET=""; C_BOLD=""; C_DIM=""; C_YEL=""; C_GRN=""; C_RED=""; C_CYN=""
fi

IS_TTY=0
[ -t 1 ] && IS_TTY=1

# Bash slices strings by character only in a multibyte locale, and the block
# glyphs are multibyte. Check before using them, or the bar draws at the wrong
# width under LC_ALL=C.
case "${LC_ALL:-${LC_CTYPE:-${LANG:-}}}" in
    *[Uu][Tt][Ff]*8*) BAR_FILL="█"; BAR_VOID="░" ;;
    *)                BAR_FILL="#"; BAR_VOID="-" ;;
esac

BAR_WIDTH=26

# --------------------------------------------------------------------------
# Progress
#
# One bar, for the whole install:
#
#     install  ██████████░░░░░░░░░░░░░░░░  38%  / cloning source (14s)
#
# It appears before the first part begins and only ever moves forward, to 100%.
# There used to be a second bar underneath for the part running now, which
# meant two numbers to read and two of them disagreeing about how far along
# things were. The part being worked on is already named in the `==> [3/5]`
# heading above and in the label beside the bar, so the second bar was saying
# nothing the screen did not already say.
#
# The figure is not a guess about elapsed time. Each of the five parts carries
# a weight (STEP_WEIGHTS) and each phase within a part owns a slice of it, with
# the measurable phases -- a download, a compile -- reporting real percentages
# from the tool doing the work.
#
# The bar lives on the bottom row and every message is printed above it, which
# is why ok/info/warn all go through note().
# --------------------------------------------------------------------------

# Where the current part has got to, 0-100.
STEP_PCT=0

# The phase inside that part: which slice of the part it owns, what to call it,
# and when it started, for the elapsed clock.
PHASE_BASE=0
PHASE_SPAN=0
PHASE_LABEL=""
# -1, not 0: SECONDS is 0 for the whole first second of the run, so a phase
# that starts promptly would look like "no phase running" and never show its
# elapsed clock.
PHASE_START=-1

# Whether the bar is currently on screen. Every write to the terminal has to
# know, because the bar is erased and redrawn rather than scrolled past.
BLOCK_SHOWN=0

# Whether there is an install to report on at all. The bar is put up before the
# first part starts -- so it is on screen from the beginning rather than
# appearing a few seconds in -- but it must stay out of the dry run, the
# banner and the uninstaller, all of which print through note() and none of
# which are an install.
PROGRESS_ON=0
SPINNER_PID=""

# Last percentage pair reported without a terminal, so a log gets one line per
# real step forward rather than one per poll.
LOG_LAST=-1

repeat_char() {
    local char="$1" count="$2" out=""
    while [ "$count" -gt 0 ]; do out="$out$char"; count=$((count - 1)); done
    printf '%s' "$out"
}

# Cached: `tput` is a fork and an exec, and a compile redraws the block often
# enough that asking the terminal its width every time is measurable. The cache
# is dropped at the start of each part, which is often enough to notice a
# window that was resized and cheap enough not to matter.
TERM_COLS=""
term_cols() {
    if [ -z "$TERM_COLS" ]; then
        local cols=""
        if [ "$IS_TTY" = 1 ]; then
            # Asked of the terminal device, not of stdout.
            #
            # This runs inside `$( )`, where stdout is a pipe -- and `tput cols`
            # measures stdout. Given a pipe it falls back to terminfo, which
            # says 80 for an xterm whatever the window is actually doing, and
            # COLUMNS is not exported to a non-interactive shell to correct it.
            # So on any window narrower than 80 every bar ran off the right
            # edge, wrapped onto a second row, and could no longer be erased by
            # the one-row erase -- leaving a dead bar behind at every phase.
            # `stty size </dev/tty` reads the terminal itself and does not care
            # what stdout is.
            cols="$( { stty size </dev/tty; } 2>/dev/null | awk '{print $2}')"
            case "$cols" in ''|*[!0-9]*) cols="$(tput cols 2>/dev/null || true)" ;; esac
        fi
        case "$cols" in ''|*[!0-9]*) cols="${COLUMNS:-80}" ;; esac
        case "$cols" in ''|*[!0-9]*) cols=80 ;; esac
        # A terminal that reports nonsense is worse than one that reports
        # nothing: a width of 0 makes every reserve negative.
        [ "$cols" -lt 20 ] && cols=80
        TERM_COLS="$cols"
    fi
    printf '%s' "$TERM_COLS"
}

# Trim a label to `width` columns.
#
# A wrapped line is not cosmetic: the bar is erased by moving the cursor up
# exactly one row, and a line that wrapped occupies two -- so one long label
# leaves the erase a row short and strands that bar on screen for good. The
# caller works out how much room there is; this only has to keep to it, and it
# has no floor of its own for that reason. Nothing is drawn at all below one
# column, which is the terminal nobody can be helped on.
fit_label() {
    local label="$1" width="$2"
    [ "$width" -lt 1 ] && return 0
    if [ "${#label}" -gt "$width" ]; then
        printf '%s…' "${label:0:$((width - 1))}"
    else
        printf '%s' "$label"
    fi
}

hide_cursor() { [ "$IS_TTY" = 1 ] && printf '\033[?25l'; return 0; }
show_cursor() { [ "$IS_TTY" = 1 ] && printf '\033[?25h'; return 0; }

bar_width() {
    local cols width="$BAR_WIDTH"
    cols="$(term_cols)"
    [ "$cols" -lt 70 ] && width=14
    [ "$cols" -lt 50 ] && width=8
    # Below forty columns the bar and its furniture fill the row on their own,
    # leaving nothing for the label -- and a row with no room left in it is a
    # row the next character wraps off the end of.
    [ "$cols" -lt 40 ] && width=4
    printf '%s' "$width"
}

# One bar: `percent` filled, in `color`.
draw_bar() {
    local percent="$1" color="$2" width filled empty
    width="$(bar_width)"
    [ "$percent" -lt 0 ]   && percent=0
    [ "$percent" -gt 100 ] && percent=100
    filled=$((percent * width / 100))
    empty=$((width - filled))
    printf '%s%s%s%s%s %3d%%' \
        "$color" "$(repeat_char "$BAR_FILL" "$filled")" \
        "$C_DIM" "$(repeat_char "$BAR_VOID" "$empty")" "$C_RESET" "$percent"
}

# How far through the whole install we are, given where the current part is.
#
# The five parts are nothing like equal in length -- building is a minute and
# checking CMake is milliseconds -- so a bar that moved a fifth per part would
# sit at 80% for almost the entire install. Each part carries a weight instead,
# and those are what the top bar counts.
overall_percent() {
    local done=0 i
    for ((i = 1; i < STEP_NUM && i <= STEP_TOTAL; i++)); do
        done=$((done + STEP_WEIGHTS[i]))
    done
    if [ "$STEP_NUM" -ge 1 ] && [ "$STEP_NUM" -le "$STEP_TOTAL" ]; then
        done=$((done + STEP_WEIGHTS[STEP_NUM] * STEP_PCT / 100))
    fi
    [ "$done" -gt 100 ] && done=100
    printf '%s' "$done"
}

# The escape sequence that erases the bar and leaves the cursor where its row
# began. Returned rather than printed, so a redraw can go out as one write --
# see block_draw.
block_erase_seq() {
    [ "$IS_TTY" = 1 ] || return 0
    [ "$BLOCK_SHOWN" = 1 ] || return 0
    printf '\033[1A\r\033[J'
}

# Erase the bar for something else to print in its place.
block_clear() {
    block_erase_seq
    BLOCK_SHOWN=0
    return 0
}

# Put the bar up at 0%, before any part has started.
progress_begin() {
    PROGRESS_ON=1
    PHASE_LABEL="starting"
    hide_cursor
    block_draw
}

# Take the bar down for good, leaving one finished bar in the scrollback.
# Everything printed after this is a plain printf that knows nothing about the
# cursor arithmetic, so the bar has to stop redrawing itself first.
progress_end() {
    STEP_PCT=100
    STEP_NUM="$STEP_TOTAL"
    block_draw
    block_clear
    PROGRESS_ON=0
    show_cursor
    [ "$IS_TTY" = 1 ] || return 0
    printf '\n    %sinstall%s  %s\n' "$C_DIM" "$C_RESET" "$(draw_bar 100 "$C_GRN")"
}

# Draw the bar. `glyph` is the spinner frame for a phase with no percentage of
# its own; blank for one that has.
block_draw() {
    local glyph="${1:- }" label elapsed=""

    [ "$PROGRESS_ON" = 1 ] || return 0

    if [ "$IS_TTY" != 1 ]; then
        # No terminal (CI, or output redirected): a bar is meaningless, so
        # report the figure at every fifth of the way instead.
        local overall
        overall="$(overall_percent)"
        if [ "$((overall / 5))" -ne "$((LOG_LAST / 5))" ]; then
            LOG_LAST="$overall"
            printf '    %3d%%  %s\n' "$overall" "$PHASE_LABEL"
        fi
        return 0
    fi

    # The clock is part of the line, so it is part of what has to be reserved.
    # It was not, and a long label plus " (12s)" ran off the right edge -- and a
    # bar that has wrapped onto a second row cannot be taken back by a one-row
    # erase, so every phase with a long label left a dead bar stranded above the
    # live one. Two of them on an 80-column terminal, which is the ordinary case.
    #
    # 22 is the fixed furniture: four spaces, "install", two spaces, the bar's
    # own trailing " 100%", two more spaces, the spinner glyph and the space
    # before the label. The extra column on top is slack -- a line that exactly
    # fills the width only stays on one row on a terminal that defers the wrap,
    # and not all of them do.
    local seconds=""
    local clock=""
    if [ "$PHASE_START" -ge 0 ]; then
        seconds="$((SECONDS - PHASE_START))"
        clock=" (${seconds}s)"
        elapsed=" $C_DIM(${seconds}s)$C_RESET"
    fi
    # What is left of the row once the bar and its fixed furniture are drawn.
    # 22 is that furniture: four spaces, "install", two spaces, the bar's own
    # trailing " 100%", two more spaces, the spinner glyph and the space before
    # the label. The extra column is slack.
    local room=$(( $(term_cols) - $(bar_width) - 23 ))

    # On a tight row the clock is the first thing to go. The label says what is
    # happening; the clock only says how long it has been happening, and four
    # columns of label is already almost nothing.
    if [ "$room" -lt $(( ${#clock} + 4 )) ]; then
        clock=""
        elapsed=""
    fi
    label="$(fit_label "$PHASE_LABEL" $(( room - ${#clock} )))"

    # Erase and redraw in a single write. Two separate printfs would leave the
    # terminal briefly showing an erased bar, which reads as a flicker on every
    # frame the spinner draws.
    printf '%s    %sinstall%s  %s  %s%s%s%s\n' \
        "$(block_erase_seq)" \
        "$C_DIM" "$C_RESET" "$(draw_bar "$(overall_percent)" "$C_GRN")" \
        "$C_CYN" "$glyph" "$C_RESET" " $label$elapsed"
    BLOCK_SHOWN=1
    return 0
}

# Print something above the block. Everything the installer says goes through
# here, so a message never lands in the middle of a bar.
note() {
    block_clear
    printf '%b\n' "$*"
    block_draw
}

# --------------------------------------------------------------------------
# Steps and phases
# --------------------------------------------------------------------------

# Begin one of the five parts.
#
# It prints nothing. The part used to announce itself with an `==> [3/5]`
# heading, which meant the one bar had five headings scrolling above it and the
# install read as a list of things that had happened rather than as one thing
# happening. The part being worked on is named beside the bar, which is the
# only place it needs to be.
step() {
    # Close the previous part out at 100% first: the phases of a part do not
    # always add up to it (an optional dependency may be skipped), and leaving
    # the bar at 85% before jumping to the next part looks like something was
    # missed rather than not needed.
    if [ "$STEP_NUM" -ge 1 ]; then
        STEP_PCT=100
        block_draw
    fi

    block_clear
    TERM_COLS=""
    STEP_NUM=$((STEP_NUM + 1))
    STEP_PCT=0
    PHASE_BASE=0
    PHASE_SPAN=0
    PHASE_LABEL="$*"
    PHASE_START=-1
    block_draw
}

# Begin a phase owning `span` percent of the current part.
phase() {
    PHASE_BASE="$STEP_PCT"
    PHASE_SPAN="$1"
    PHASE_LABEL="$2"
    PHASE_START="$SECONDS"
    block_draw
}

# Report progress inside a measurable phase, as 0-100 of the phase itself.
phase_at() {
    local fraction="$1"
    [ "$fraction" -lt 0 ]   && fraction=0
    [ "$fraction" -gt 100 ] && fraction=100
    local target=$((PHASE_BASE + PHASE_SPAN * fraction / 100))
    [ "$target" -gt 100 ] && target=100
    # Never go backwards. cmake restarts its percentage for each target it
    # builds, and a bar that retreats reads as a failure.
    [ "$target" -le "$STEP_PCT" ] && return 0
    STEP_PCT="$target"
    # Redrawing per line rather than per percentage would mean thousands of
    # redraws for a hundred pixels of bar, and a visible flicker.
    block_draw
}

# Finish the current phase, optionally saying what it achieved.
phase_end() {
    local message="${1:-}"
    STEP_PCT=$((PHASE_BASE + PHASE_SPAN))
    [ "$STEP_PCT" -gt 100 ] && STEP_PCT=100
    PHASE_START=-1
    block_draw
    [ -n "$message" ] && ok "$message"
    return 0
}

# The animation for a phase with no percentage of its own -- a package
# manager, a configure run. The block is redrawn from a background process
# because the foreground is blocked on the command; nothing else writes to the
# terminal while this runs, so the two cannot fight over the cursor.
spinner_start() {
    [ "$IS_TTY" = 1 ] || return 0
    (
        frames='|/-\'
        i=0
        while :; do
            block_draw "${frames:$((i % 4)):1}"
            i=$((i + 1))
            sleep 0.15
        done
    ) &
    SPINNER_PID=$!
}

spinner_stop() {
    if [ -n "$SPINNER_PID" ]; then
        kill "$SPINNER_PID" 2>/dev/null || true
        wait "$SPINNER_PID" 2>/dev/null || true
        SPINNER_PID=""
    fi
    # The background process left the block on screen; the foreground's own
    # idea of that is what the next block_clear reads.
    BLOCK_SHOWN=1
    return 0
}


human_bytes() {
    local bytes="${1:-0}"
    if   [ "$bytes" -ge 1073741824 ]; then printf '%d.%dGB' $((bytes / 1073741824)) $(((bytes % 1073741824) * 10 / 1073741824))
    elif [ "$bytes" -ge 1048576 ];    then printf '%dMB' $((bytes / 1048576))
    elif [ "$bytes" -ge 1024 ];       then printf '%dKB' $((bytes / 1024))
    else printf '%dB' "$bytes"; fi
}

# Leaving the cursor hidden after a Ctrl-C would break the user's terminal.
cleanup() {
    [ -n "$SPINNER_PID" ] && { kill "$SPINNER_PID" 2>/dev/null || true; }
    show_cursor
}
trap cleanup EXIT INT TERM

STEP_NUM=0
STEP_TOTAL=5

# --------------------------------------------------------------------------
# How long each part takes, relative to the others.
#
# These are rough measurements of a cold install on a mid-range machine, not
# guesses; they only have to be right about the shape. Part 5 dominates because
# it compiles Crucible, and part 2 is second because installing a toolchain (and
# possibly a Vulkan SDK) is the only other thing here that touches the network
# for minutes at a time.
#
# Indexed by step number, so [0] is unused and the rest sum to 100.
# --------------------------------------------------------------------------
STEP_WEIGHTS=(0 2 26 2 8 62)


# Everything the installer says goes through note(), warnings included.
#
# A warning written straight to stderr while the bar is on screen would land
# under it and leave the cursor arithmetic a row out, so the display and the
# message would both be wrong from then on. Only die() writes to stderr
# directly, and it retires the block first because it is the last thing said.
#
# While the bar is up, the bar is all there is. info(), ok() and muted() are
# running commentary -- what was detected, what was installed, what a phase
# achieved -- and every one of them was already said by the label beside the
# bar, in the moment it was true. Printing them as well turned a one-line
# display into forty lines of scrollback with a bar at the bottom of it, which
# is not a progress bar, it is a log with a bar in it. They still speak in the
# dry run and the uninstaller, where there is no bar to speak for them.
#
# warn() is exempt on purpose: it says something is not as asked -- a desktop
# app that cannot be built here, a package that would not install -- and that
# has to reach the screen whatever else is on it.
info()  { if [ "$PROGRESS_ON" != 1 ]; then note "    $*"; fi; }
muted() { if [ "$PROGRESS_ON" != 1 ]; then note "    $C_DIM$*$C_RESET"; fi; }
warn()  { note "$C_YEL !! $C_RESET $*"; }
ok()    { if [ "$PROGRESS_ON" != 1 ]; then note "    ${C_GRN}✓${C_RESET} $*"; fi; }
die()   { block_clear; show_cursor; printf '\n%serror:%s %s\n' "$C_RED" "$C_RESET" "$*" >&2; exit 1; }

banner() {
# packaging/flame.txt, verbatim. Quoted here-doc, so nothing in it is
# expanded, and UTF-8 braille rather than ASCII because every terminal a
# shell installer runs in has had a Unicode font for fifteen years.
cat <<'ART'

      ⠀⠀⠀⠀⠀⠀⢱⣆⠀⠀⠀⠀⠀⠀
      ⠀⠀⠀⠀⠀⠀⠈⣿⣷⡀⠀⠀⠀⠀
      ⠀⠀⠀⠀⠀⠀⢸⣿⣿⣷⣧⠀⠀⠀
      ⠀⠀⠀⠀⡀⢠⣿⡟⣿⣿⣿⡇⠀⠀
      ⠀⠀⠀⠀⣳⣼⣿⡏⢸⣿⣿⣿⢀⠀   Crucible
      ⠀⠀⠀⣰⣿⣿⡿⠁⢸⣿⣿⡟⣼⡆   a local LLM engine that delegates.
      ⢰⢀⣾⣿⣿⠟⠀⠀⣾⢿⣿⣿⣿⣿
      ⢸⣿⣿⣿⡏⠀⠀⠀⠃⠸⣿⣿⣿⡿
      ⢳⣿⣿⣿⠀⠀⠀⠀⠀⠀⢹⣿⡿⡁
      ⠀⠹⣿⣿⡄⠀⠀⠀⠀⠀⢠⣿⡞⠁
      ⠀⠀⠈⠛⢿⣄⠀⠀⠀⣠⠞⠋⠀⠀
      ⠀⠀⠀⠀⠀⠀⠉⠀⠀⠀⠀⠀⠀⠀

ART
}

usage() {
    banner
    cat <<EOF
usage: install.sh [options]

  Installs the program and nothing else: crucible, crucible-gui, and the
  directories they keep their files in. Compute runtimes are not an installer's
  business -- Crucible builds one on demand from its settings screen, on the
  machine that will run it.

  --prefix DIR     install location (default: /usr/local with sudo,
                   otherwise ~/.local)
  --branch NAME    git branch to build (default: main)
  --jobs N         parallel build jobs (default: all cores)
  --no-gui         build only the terminal program, skipping crucible-gui.
                   The desktop app is built by default; it needs OpenGL and,
                   on Linux, the X11 development headers, which this installs.
                   Where they are unavailable the build falls back to the
                   terminal program on its own.
  --gui            build the desktop application. On by default; the flag is
                   kept so an explicit --gui still means what it says.
  --no-deps        do not install system packages
  -y, --yes        assume yes; never prompt
  --check          report what would be installed, then exit without
                   changing anything
  --uninstall      remove Crucible and everything it installed: both
                   programs, the libraries, the menu entry, the config, the
                   folder-trust list, the models, the runtimes and the history
  -h, --help       this message

  Windows has its own one-command installer:
    irm https://raw.githubusercontent.com/mattsaund/Crucible/main/install.ps1 | iex

examples:
  curl -fsSL $RAW_URL | bash
  curl -fsSL $RAW_URL | bash -s -- --uninstall
  ./install.sh --prefix ~/.local
  ./install.sh --no-gui
EOF
}

# --------------------------------------------------------------------------
# Arguments
# --------------------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --gui)       WITH_GUI=1;       shift ;;
        --no-gui)    WITH_GUI=0;       shift ;;
        --prefix)    PREFIX="${2:-}"; shift 2 ;;
        --prefix=*)  PREFIX="${1#*=}";shift ;;
        --branch)    BRANCH="${2:-}"; shift 2 ;;
        --branch=*)  BRANCH="${1#*=}";shift ;;
        --jobs)      JOBS="${2:-}";   shift 2 ;;
        --jobs=*)    JOBS="${1#*=}";  shift ;;
        --no-deps)   INSTALL_DEPS=0;  shift ;;
        -y|--yes)    ASSUME_YES=1;    shift ;;
        --uninstall) DO_UNINSTALL=1;  shift ;;
        --check)     DO_CHECK=1;      shift ;;
        -h|--help)   usage; exit 0 ;;
        *) die "unknown option '$1' (try --help)" ;;
    esac
done

# --------------------------------------------------------------------------
# Prompting
#
# The script is usually running as `curl | bash`, so stdin is the script itself
# and `read` would consume it. Questions go to the terminal directly, and when
# there is no terminal we take the safe default rather than hanging.
# --------------------------------------------------------------------------
# Is there a terminal to ask a question on?
#
# `[ -r /dev/tty ]` is not the test. The device node is there and readable by
# permission on a process with no controlling terminal, and it is the open that
# then fails with ENXIO -- which is exactly the case this has to detect, since
# it is what a detached shell, a CI runner and a pipeline all look like. So the
# open is what gets tried, in a subshell, where failing costs nothing.
have_tty() {
    ( exec </dev/tty ) 2>/dev/null
}

confirm() {
    local prompt="$1" default="${2:-y}" reply
    if [ "$ASSUME_YES" = 1 ]; then return 0; fi
    if ! have_tty; then
        muted "no terminal to ask on; assuming '$default'"
        [ "$default" = "y" ]
        return
    fi
    local hint="[Y/n]"; [ "$default" = "n" ] && hint="[y/N]"
    # Asked on /dev/tty, so it never lands in a redirected log -- and after
    # the block is out of the way, since the answer is typed where it was.
    block_clear
    show_cursor
    printf '    %s %s ' "$prompt" "$hint" > /dev/tty
    read -r reply < /dev/tty || reply=""
    reply="${reply:-$default}"
    case "$reply" in [yY]|[yY][eE][sS]) return 0 ;; *) return 1 ;; esac
}

# --------------------------------------------------------------------------
# Platform
# --------------------------------------------------------------------------

# macOS. Homebrew for the toolchain, Metal for the GPU, and no sudo anywhere:
# brew refuses to run as root, and nothing else here needs it once the prefix
# is somewhere the user owns.
detect_platform_macos() {
    SUDO=""
    PKG="brew"

    if ! command -v brew >/dev/null 2>&1; then
        PKG="unknown"
        if [ "$INSTALL_DEPS" = 1 ]; then
            warn "Homebrew not found; skipping package installation."
            warn "install it from https://brew.sh, or pass --no-deps and provide cmake yourself."
            INSTALL_DEPS=0
        fi
    fi

    # The compiler, the SDK and the Metal shader compiler all arrive together
    # in the Xcode command line tools, and Homebrew needs them too -- so this
    # is the one dependency that cannot be installed by the package manager.
    if ! xcode-select -p >/dev/null 2>&1; then
        warn "the Xcode command line tools are not installed."
        warn "run:  xcode-select --install"
    fi

    info "platform     : macOS $(sw_vers -productVersion 2>/dev/null || echo unknown) ($(uname -m))"
    info "package tool : $PKG"
}

detect_platform() {
    OS="$(uname -s)"
    case "$OS" in
        Linux|Darwin) ;;
        *) die "this installer supports Linux and macOS (found $OS)." ;;
    esac

    # macOS takes a different route through everything below: Homebrew rather
    # than a system package manager, and Metal rather than CUDA or Vulkan. It
    # is handled first so the Linux path stays exactly as it was.
    if [ "$OS" = "Darwin" ]; then
        detect_platform_macos
        return
    fi

    if [ "$(id -u)" -ne 0 ]; then
        if command -v sudo >/dev/null 2>&1; then
            SUDO="sudo"
        else
            SUDO=""
            if [ "$INSTALL_DEPS" = 1 ]; then
                warn "not root and no sudo found; skipping package installation."
                INSTALL_DEPS=0
            fi
        fi
    fi

    local id="" like=""
    if [ -r /etc/os-release ]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        id="${ID:-}"; like="${ID_LIKE:-}"
    fi

    case "$id $like" in
        *debian*|*ubuntu*|*mint*)               PKG="apt" ;;
        *fedora*|*rhel*|*centos*)               PKG="dnf" ;;
        *arch*|*manjaro*|*endeavouros*)         PKG="pacman" ;;
        *suse*)                                 PKG="zypper" ;;
        *)
            if   command -v apt-get >/dev/null 2>&1; then PKG="apt"
            elif command -v dnf     >/dev/null 2>&1; then PKG="dnf"
            elif command -v pacman  >/dev/null 2>&1; then PKG="pacman"
            elif command -v zypper  >/dev/null 2>&1; then PKG="zypper"
            else PKG="unknown"; fi ;;
    esac

    info "distribution : ${PRETTY_NAME:-unknown}"
    info "package tool : $PKG"

    # Ask for the sudo password now, while nothing is being drawn over. A
    # password prompt appearing underneath a running spinner is unreadable and
    # looks like a hang.
    if [ -n "$SUDO" ] && [ "$INSTALL_DEPS" = 1 ]; then
        if ! sudo -n true 2>/dev/null; then
            info "administrator access is needed to install packages"
            sudo -v || die "could not obtain sudo; re-run with --no-deps to skip package installation"
        fi
    fi

    if [ "$PKG" = "unknown" ] && [ "$INSTALL_DEPS" = 1 ]; then
        warn "unrecognised package manager; skipping dependency installation."
        warn "you will need: a C++20 compiler, cmake >= 3.24, git."
        INSTALL_DEPS=0
    fi
}

PKG_LOG=""

pkg_install() {
    [ $# -gt 0 ] || return 0
    local status=0
    PKG_LOG="$(mktemp)"

    # Package managers report progress in their own incompatible ways and none
    # of it is reliably parseable, so this phase animates with an elapsed clock
    # rather than showing a bar that would have to invent the percentage.
    PHASE_LABEL="installing $*"
    spinner_start
    case "$PKG" in
        apt)    { $SUDO apt-get update -qq \
                  && DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y -qq "$@"; } \
                  > "$PKG_LOG" 2>&1 || status=$? ;;
        brew)   brew install "$@"                          > "$PKG_LOG" 2>&1 || status=$? ;;
        dnf)    $SUDO dnf install -y -q "$@"                > "$PKG_LOG" 2>&1 || status=$? ;;
        pacman) $SUDO pacman -Sy --needed --noconfirm "$@"  > "$PKG_LOG" 2>&1 || status=$? ;;
        zypper) $SUDO zypper --non-interactive install -y "$@" > "$PKG_LOG" 2>&1 || status=$? ;;
        *)      spinner_stop; warn "cannot install $* automatically"; return 1 ;;
    esac
    spinner_stop

    if [ "$status" -eq 0 ]; then
        ok "installed $*"
    else
        warn "package installation failed:"
        tail -8 "$PKG_LOG" >&2 || true
    fi
    rm -f "$PKG_LOG"
    return "$status"
}

# Is a package available to install at all? Keeps us from failing the whole run
# on a package that this distro release simply does not carry.
#
# The output is captured and then matched, rather than piped into `grep -q`.
# grep -q exits at the first match, which hands the package tool a SIGPIPE, and
# under `set -o pipefail` the pipeline then reports 141 -- so every package
# looked unavailable however installable it really was. That is how the desktop
# app came to be skipped on every apt system: nothing was missing, the question
# was just being asked in a way that could only ever answer "no".
pkg_available() {
    local out
    case "$PKG" in
        apt)    out="$(apt-cache policy "$1" 2>/dev/null)" || return 1
                case "$out" in
                    *'Candidate: (none)'*) return 1 ;;
                    *'Candidate: '*)       return 0 ;;
                    *)                     return 1 ;;
                esac ;;
        brew)   brew info --formula "$1" >/dev/null 2>&1 ;;
        dnf)    dnf list --available "$1" >/dev/null 2>&1 || dnf list --installed "$1" >/dev/null 2>&1 ;;
        pacman) pacman -Si "$1" >/dev/null 2>&1 ;;
        zypper) out="$(zypper --non-interactive info "$1" 2>/dev/null)" || return 1
                case "$out" in
                    Version*|*"
"Version*) return 0 ;;
                    *)         return 1 ;;
                esac ;;
        *)      return 1 ;;
    esac
}

# --------------------------------------------------------------------------
# CMake
#
# Crucible needs CMake >= 3.24, which is newer than several current LTS releases
# ship. Rather than fail, fetch the official static build into a cache dir.
# --------------------------------------------------------------------------
cmake_version_ok() {
    local exe="$1" version major minor
    command -v "$exe" >/dev/null 2>&1 || return 1
    version="$("$exe" --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1)"
    [ -n "$version" ] || return 1
    major="${version%%.*}"; minor="${version#*.}"; minor="${minor%%.*}"
    [ "$major" -gt "$CMAKE_MIN_MAJOR" ] && return 0
    [ "$major" -eq "$CMAKE_MIN_MAJOR" ] && [ "$minor" -ge "$CMAKE_MIN_MINOR" ]
}

# Download with a real progress bar.
#
# curl's own --progress-bar cannot be restyled and writes to stderr in a format
# that is not worth parsing, so the transfer runs in the background and the bar
# is driven by the size of the file on disk against Content-Length. Without a
# Content-Length (chunked responses) it degrades to a spinner rather than
# inventing a percentage.
download_with_progress() {
    local url="$1" out="$2" label="$3"
    local total="" pid status=0 now=0

    total="$(curl -fsSLI "$url" 2>/dev/null \
        | awk 'BEGIN{IGNORECASE=1} /^content-length:/ {v=$2} END{gsub(/[^0-9]/,"",v); print v}')"

    rm -f "$out"
    curl -fsSL "$url" -o "$out" &
    pid=$!

    if [ -n "$total" ] && [ "$total" -gt 0 ] 2>/dev/null && [ "$IS_TTY" = 1 ]; then
        while kill -0 "$pid" 2>/dev/null; do
            now=0
            # stat's flags differ between GNU and BSD, so ask both. wc is the
            # last resort and is everywhere.
            [ -f "$out" ] && now="$(stat -c %s "$out" 2>/dev/null \
                                    || stat -f %z "$out" 2>/dev/null \
                                    || wc -c < "$out" 2>/dev/null || echo 0)"
            PHASE_LABEL="$label  $(human_bytes "$now") / $(human_bytes "$total")"
            phase_at "$((now * 100 / total))"
            sleep 0.15
        done
        wait "$pid" || status=$?
        [ "$status" -eq 0 ] && PHASE_LABEL="$label  $(human_bytes "$total")" && phase_at 100
    else
        PHASE_LABEL="downloading $label"
        spinner_start
        wait "$pid" || status=$?
        spinner_stop
    fi
    [ "$status" -eq 0 ] && ok "downloaded $label"

    return "$status"
}

bootstrap_cmake() {
    local arch cache url tarball dir
    arch="$(uname -m)"
    case "$arch" in
        x86_64|amd64)  arch="x86_64" ;;
        aarch64|arm64) arch="aarch64" ;;
        *) die "no prebuilt CMake for $arch; please install cmake >= ${CMAKE_MIN_MAJOR}.${CMAKE_MIN_MINOR} yourself." ;;
    esac

    cache="${XDG_CACHE_HOME:-$HOME/.cache}/crucible"
    dir="$cache/cmake-${CMAKE_BOOTSTRAP_VERSION}-linux-${arch}"
    mkdir -p "$cache"

    if [ ! -x "$dir/bin/cmake" ]; then
        url="https://github.com/Kitware/CMake/releases/download/v${CMAKE_BOOTSTRAP_VERSION}/cmake-${CMAKE_BOOTSTRAP_VERSION}-linux-${arch}.tar.gz"
        tarball="$cache/cmake.tar.gz"
        info "your CMake is older than ${CMAKE_MIN_MAJOR}.${CMAKE_MIN_MINOR}; fetching ${CMAKE_BOOTSTRAP_VERSION}"
        phase 80 "downloading CMake ${CMAKE_BOOTSTRAP_VERSION}"
        download_with_progress "$url" "$tarball" "CMake ${CMAKE_BOOTSTRAP_VERSION}" \
            || die "could not download CMake from $url"
        phase_end

        phase 20 "extracting CMake"
        spinner_start
        tar -xzf "$tarball" -C "$cache"
        spinner_stop
        phase_end "extracted"
        rm -f "$tarball"
    fi

    [ -x "$dir/bin/cmake" ] || die "CMake bootstrap failed"
    CMAKE="$dir/bin/cmake"
    ok "using $($CMAKE --version | head -1) from cache"
}

ensure_cmake() {
    if cmake_version_ok cmake; then
        CMAKE="cmake"
        ok "$(cmake --version | head -1)"
    else
        bootstrap_cmake
    fi
}

# --------------------------------------------------------------------------
# Dependencies
# --------------------------------------------------------------------------
PKGS_BASE=()
PKGS_GUI=()

# Which packages this distribution needs. Two lists, and only two: a C++
# toolchain, and the headers the desktop app links against. There used to be a
# Vulkan SDK and a CUDA toolkit here as well -- several gigabytes of them, in
# the CUDA case -- installed so that a GPU runtime could be *built later*. The
# program installs its own SDKs when you ask it for a runtime, on the machine
# that will run it, so this was fetching a compiler for a compile that may
# never happen.
#
# Split out from the install so --check can report them without touching
# anything.
resolve_packages() {
    PKGS_BASE=(); PKGS_GUI=()
    case "$PKG" in
        apt)    PKGS_BASE=(build-essential cmake git pkg-config curl ca-certificates)
                # The desktop app. GLFW itself is preferred from the system;
                # these are what it needs either way, and what building it from
                # source needs when the distribution has no package.
                PKGS_GUI=(libgl1-mesa-dev libglfw3-dev libx11-dev libxrandr-dev
                          libxinerama-dev libxcursor-dev libxi-dev) ;;
        dnf)    PKGS_BASE=(gcc-c++ make cmake git pkgconf-pkg-config curl)
                PKGS_GUI=(mesa-libGL-devel glfw-devel libX11-devel libXrandr-devel
                          libXinerama-devel libXcursor-devel libXi-devel) ;;
        pacman) PKGS_BASE=(base-devel cmake git curl)
                PKGS_GUI=(mesa glfw libx11 libxrandr libxinerama libxcursor libxi) ;;
        zypper) PKGS_BASE=(gcc-c++ make cmake git-core curl)
                PKGS_GUI=(Mesa-libGL-devel libglfw-devel libX11-devel libXrandr-devel
                          libXinerama-devel libXcursor-devel libXi-devel) ;;
        # macOS: the compiler, git, curl and OpenGL all come with the system
        # or the command line tools. Only cmake is actually missing, and GLFW
        # is built from source because Homebrew's is not always there.
        brew)   PKGS_BASE=(cmake)
                PKGS_GUI=() ;;
    esac
    return 0
}

# What the build needs, and nothing more: a C++ toolchain, and the headers the
# desktop app links against.
install_dependencies() {
    resolve_packages

    phase 65 "installing the build toolchain"
    pkg_install ${PKGS_BASE[@]+"${PKGS_BASE[@]}"} || die "could not install the build toolchain"
    phase_end

    # The desktop app's dependencies. Small -- a few megabytes of headers --
    # but they are the one thing Crucible needs from the system beyond a
    # compiler, so this is the one step that can fail on a machine that is
    # otherwise fine.
    #
    # The set is filtered to what this release actually carries rather than
    # installed all-or-nothing. Package names drift between releases -- they get
    # renamed, split, or turned into transitional stubs and dropped -- and one
    # name going missing is no reason to withhold the desktop app when every
    # header it needs is present under some other name. Whether it can really be
    # built is settled afterwards by verify_gui_prerequisites, which looks for
    # the headers themselves.
    #
    # Skipped entirely on macOS, where PKGS_GUI is empty because OpenGL comes
    # with the system.
    if [ "$WITH_GUI" = "1" ] && [ "${#PKGS_GUI[@]}" -gt 0 ]; then
        phase 35 "installing the desktop app's dependencies"
        local gui_have=() gui_missing=() gui_pkg
        for gui_pkg in ${PKGS_GUI[@]+"${PKGS_GUI[@]}"}; do
            if pkg_available "$gui_pkg"; then
                gui_have+=("$gui_pkg")
            else
                gui_missing+=("$gui_pkg")
            fi
        done
        if [ "${#gui_missing[@]}" -gt 0 ]; then
            muted "not packaged on this release: ${gui_missing[*]-}"
        fi
        if [ "${#gui_have[@]}" -gt 0 ]; then
            pkg_install ${gui_have[@]+"${gui_have[@]}"} ||
                warn "some desktop packages did not install; checking for the headers anyway"
        else
            warn "none of the desktop packages are on this distribution"
        fi
        phase_end
    fi
    return 0
}

# Is a header on the include path?
#
# The multiarch directory is where Debian and Ubuntu put most of these, and it
# is named after the machine, so it has to be built rather than listed.
have_header() {
    local header="$1" dir
    for dir in /usr/include \
               "/usr/include/$(uname -m)-linux-gnu" \
               /usr/local/include \
               /opt/homebrew/include; do
        [ -f "$dir/$header" ] && return 0
    done
    return 1
}

# Can the desktop app actually be built here?
#
# Asked of the filesystem rather than of the package manager, and asked whether
# or not the dependency step ran -- so --no-deps gets an honest answer too, and
# a machine that already had the headers is not refused the desktop app because
# a package name it never needed is missing.
#
# What the build genuinely requires is GL/gl.h (find_package(OpenGL REQUIRED)),
# plus either a system GLFW or the X11 headers, since CrucibleDependencies.cmake
# compiles GLFW from source when the system has none.
verify_gui_prerequisites() {
    [ "$WITH_GUI" = "1" ] || return 0
    # macOS: OpenGL is part of the system and there is no X11 in it.
    [ "$(uname -s)" = "Darwin" ] && return 0

    if ! have_header GL/gl.h; then
        warn "the OpenGL headers (GL/gl.h) are not installed"
        warn "building the terminal program only; install them and re-run for the desktop app"
        WITH_GUI=0
        return 0
    fi
    if ! have_header GLFW/glfw3.h && ! have_header X11/Xlib.h; then
        warn "neither GLFW nor the X11 headers are installed, and GLFW needs them to build"
        warn "building the terminal program only; install them and re-run for the desktop app"
        WITH_GUI=0
        return 0
    fi
    return 0
}

# --------------------------------------------------------------------------
# Source
# --------------------------------------------------------------------------
# git reports "Receiving objects:  45% (90/200)" on stderr under --progress,
# which is a genuine percentage worth showing on a slow connection.
git_clone_with_progress() {
    local url="$1" dest="$2" branch="$3" status=0 log
    log="$(mktemp)"

    set +e
    git clone --depth 1 --branch "$branch" --progress "$url" "$dest" 2>&1 \
        | while IFS= read -r line; do
              printf '%s\n' "$line" >> "$log"
              case "$line" in
                  *"Receiving objects:"*|*"Resolving deltas:"*)
                      percent="${line#*: }"
                      percent="${percent%%\%*}"
                      percent="${percent// /}"
                      case "$percent" in
                          ''|*[!0-9]*) ;;
                          *) PHASE_LABEL="${line%%:*}"; phase_at "$percent" ;;
                      esac
                      ;;
              esac
          done
    status="${PIPESTATUS[0]}"
    set -e

    if [ "$status" -ne 0 ]; then
        show_log_tail "$log" 15
        rm -f "$log"
        die "could not clone $url"
    fi
    rm -f "$log"
    ok "cloned $branch"
}

locate_source() {
    local here=""
    if [ -n "${BASH_SOURCE[0]:-}" ] && [ -f "${BASH_SOURCE[0]:-}" ]; then
        here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    fi

    # Running ./install.sh from inside a checkout builds that checkout, so you
    # can test local changes without pushing them first.
    if [ -n "$here" ] && [ -f "$here/CMakeLists.txt" ] && grep -q 'project(crucible' "$here/CMakeLists.txt" 2>/dev/null; then
        SRC_DIR="$here"
        info "building the checkout at $SRC_DIR"
        return
    fi

    SRC_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/crucible/src"
    if [ -d "$SRC_DIR/.git" ]; then
        PHASE_LABEL="updating $SRC_DIR"
        spinner_start
        git -C "$SRC_DIR" fetch --depth 1 origin "$BRANCH" --quiet >/dev/null 2>&1
        git -C "$SRC_DIR" checkout --quiet FETCH_HEAD >/dev/null 2>&1
        spinner_stop
        ok "updated to latest $BRANCH"
    else
        info "cloning into $SRC_DIR"
        mkdir -p "$(dirname "$SRC_DIR")"
        rm -rf "$SRC_DIR"
        git_clone_with_progress "$REPO_URL" "$SRC_DIR" "$BRANCH"
        CLONED_FRESH=1
    fi
}

# --------------------------------------------------------------------------
# Build & install
# --------------------------------------------------------------------------
choose_prefix() {
    if [ -n "$PREFIX" ]; then return; fi
    if [ "$(id -u)" -eq 0 ] || [ -n "$SUDO" ]; then
        PREFIX="/usr/local"
    else
        PREFIX="$HOME/.local"
    fi
}

BUILD_LOG=""

# Show why a step failed without burying the user in thousands of lines.
show_log_tail() {
    local log="$1" lines="${2:-25}"
    [ -f "$log" ] || return 0
    # Straight to stderr, bypassing note(), so retire the block first: anything
    # printed under it leaves the cursor arithmetic a row out and every later
    # redraw erases real output.
    block_clear
    show_cursor

    # The end of the log is not reliably where the error is. CMake writes its
    # status lines to stdout, which libc block-buffers when it is a file, and
    # its errors to stderr, which it does not -- so the two arrive out of order,
    # and a build killed part way through loses whatever stdout had buffered.
    # Either way the tail can be 25 lines of cheerful progress with the actual
    # failure sitting further up, which is worse than useless in a bug report.
    # So the error is looked for by name first, and the tail is what follows.
    local hits
    hits="$(grep -n -E 'CMake Error|error:|Error [0-9]+|FAILED:|No such file' "$log" 2>/dev/null | head -20)"
    if [ -n "$hits" ]; then
        printf '\n%s--- errors in %s ---%s\n' "$C_DIM" "$log" "$C_RESET" >&2
        printf '%s\n' "$hits" >&2
    fi

    printf '\n%s--- last %d lines of %s ---%s\n' "$C_DIM" "$lines" "$log" "$C_RESET" >&2
    tail -n "$lines" "$log" >&2
    printf '%s--- end ---%s\n\n' "$C_DIM" "$C_RESET" >&2
}

# Where to build: beside the source, always.
#
# This used to probe the filesystem for symlink support and move the build tree
# into the cache directory when there was none, because llama.cpp's shared
# libraries were written under a versioned name with an unversioned symlink
# beside them -- and exFAT and NTFS cannot hold a symlink, so the link step
# failed a long way into the build. Crucible now builds those libraries without
# the version suffix (see cmake/CrucibleUnversion.cmake), so there is nothing
# left to detect and the build tree can stay where the user put the checkout.
choose_build_dir() {
    BUILD_DIR="$SRC_DIR/build"
    mkdir -p "$BUILD_DIR"
    return 0
}

# The configure invocation, in one place so the retry below cannot drift from
# the first attempt.
run_configure() {
    "$CMAKE" -S "$SRC_DIR" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCRUCIBLE_BACKEND_DL=ON \
        -DCRUCIBLE_BUILD_GUI="$([ "$WITH_GUI" = "1" ] && echo ON || echo OFF)" \
        > "$BUILD_LOG" 2>&1 || status=$?
    return 0
}

build_and_install() {
    local status=0
    choose_build_dir
    local build_dir="$BUILD_DIR"

    # The log is only interesting when something breaks, so its path is
    # announced on failure rather than upfront, and it is removed on success
    # instead of accumulating in /tmp on every upgrade.
    BUILD_LOG="$(mktemp -t crucible-build-XXXXXX.log)"

    # --- configure ---------------------------------------------------------
    # The first configure clones llama.cpp and FTXUI, so it is slow and has no
    # percentage of its own.
    # No GPU backend is compiled in. Crucible is built with ggml's loadable
    # backends, so a compute backend is a file the settings screen manages
    # rather than a decision frozen here.
    phase 12 "configuring"
    spinner_start
    run_configure
    spinner_stop

    # Configure failed with a build directory already there. By far the most
    # likely reason is a CMakeCache.txt remembering paths this tree no longer
    # has -- a moved home directory, a restored backup, a source directory that
    # was pointed somewhere else once -- and cmake refuses to reuse it. Since
    # the build tree now stays in the checkout rather than being relocated to a
    # fresh cache directory, re-running the installer over an old one is the
    # ordinary case rather than a rare one.
    #
    # The cache is pure derived data, so throwing it away and trying once more
    # is both safe and the fix the user would otherwise have to find out about
    # from a log. The runtime builder does the same thing for the same reason.
    if [ "$status" -ne 0 ] && [ -f "$build_dir/CMakeCache.txt" ]; then
        warn "the existing build directory is stale; clearing it and trying again"
        rm -rf "$build_dir"
        mkdir -p "$build_dir"
        status=0
        spinner_start
        run_configure
        spinner_stop
    fi

    if [ "$status" -ne 0 ]; then
        show_log_tail "$BUILD_LOG"
        die "cmake configure failed. The full log is at $BUILD_LOG"
    fi
    phase_end "configured"

    # --- compile -----------------------------------------------------------
    # CMake's Makefile generator prints "[ 42%] Building ..." for every unit,
    # which is a real, ordered percentage worth turning into a bar. The full
    # output still goes to the log so a failure can be diagnosed.
    info "compiling with $JOBS jobs"
    phase 70 "compiling"
    status=0
    set +e
    "$CMAKE" --build "$build_dir" -j "$JOBS" 2>&1 \
        | while IFS= read -r line; do
              printf '%s\n' "$line" >> "$BUILD_LOG"
              case "$line" in
                  \[*%\]*)
                      percent="${line#*[}"
                      percent="${percent%%\%*}"
                      percent="${percent// /}"
                      case "$percent" in
                          ''|*[!0-9]*) ;;
                          *)
                              target="${line#*] }"
                              PHASE_LABEL="${target:0:40}"
                              phase_at "$percent"
                              ;;
                      esac
                      ;;
              esac
          done
    status="${PIPESTATUS[0]}"
    set -e

    if [ "$status" -ne 0 ]; then
        show_log_tail "$BUILD_LOG" 30
        die "build failed. The full log is at $BUILD_LOG"
    fi
    phase_end "compiled"

    # --- tests -------------------------------------------------------------
    phase 6 "running tests"
    spinner_start
    status=0
    (cd "$build_dir" && ctest --output-on-failure >> "$BUILD_LOG" 2>&1) || status=$?
    spinner_stop
    if [ "$status" -eq 0 ]; then
        phase_end "tests passed"
    else
        phase_end
        warn "tests did not pass; installing anyway (please report this)"
        warn "details are in $BUILD_LOG"
        KEEP_BUILD_LOG=1
    fi

    # --- install -----------------------------------------------------------
    phase 6 "installing to ${PREFIX}/bin"
    spinner_start
    status=0
    if [ -w "$PREFIX" ] || [ "$(id -u)" -eq 0 ]; then
        "$CMAKE" --install "$build_dir" --component crucible >> "$BUILD_LOG" 2>&1 || status=$?
    else
        mkdir -p "$PREFIX/bin" 2>/dev/null || true
        if [ -w "$PREFIX/bin" ]; then
            "$CMAKE" --install "$build_dir" --component crucible >> "$BUILD_LOG" 2>&1 || status=$?
        else
            $SUDO "$CMAKE" --install "$build_dir" --component crucible >> "$BUILD_LOG" 2>&1 || status=$?
        fi
    fi
    spinner_stop
    if [ "$status" -ne 0 ]; then
        show_log_tail "$BUILD_LOG"
        die "install failed"
    fi
    phase_end "installed"

    # The summary at the end reports what was installed, so it has to be told
    # the truth rather than repeating what was asked for. A desktop app that was
    # configured, compiled and then not installed is a bug worth naming here
    # instead of printing a path to a file that is not there.
    if [ "$WITH_GUI" = "1" ] && [ ! -x "$PREFIX/bin/crucible-gui" ]; then
        warn "crucible-gui was built but is not at $PREFIX/bin/crucible-gui"
        warn "details are in $BUILD_LOG"
        KEEP_BUILD_LOG=1
        WITH_GUI=0
    fi

    # Tell the desktop about the new application-menu entry.
    #
    # Both tools are caches, both are optional, and neither failing is worth a
    # word: the entry is on disk either way and every desktop picks it up on the
    # next login. Running them just means the icon appears now rather than then.
    if [ "$WITH_GUI" = "1" ] && [ "$(uname -s)" != "Darwin" ] \
       && [ -f "$PREFIX/share/applications/crucible.desktop" ]; then
        command -v update-desktop-database >/dev/null 2>&1 \
            && update-desktop-database "$PREFIX/share/applications" >/dev/null 2>&1
        command -v gtk-update-icon-cache >/dev/null 2>&1 \
            && gtk-update-icon-cache -qtf "$PREFIX/share/icons/hicolor" >/dev/null 2>&1
        ok "added Crucible to the application menu"
    fi

    if [ -z "${KEEP_BUILD_LOG:-}" ]; then
        rm -f "$BUILD_LOG"
        BUILD_LOG=""
    fi
}

# --------------------------------------------------------------------------
# Where Crucible keeps its files
#
# Created by the installer so the first run opens onto directories that are
# already there, and so `--uninstall` has one place to look. Nothing is put in
# them: no models, and no compute runtime. Both are the program's to manage.
# --------------------------------------------------------------------------
CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/crucible"
DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/crucible"
MODELS_DIR="$DATA_DIR/models"

make_directories() {
    mkdir -p "$CONFIG_DIR" "$DATA_DIR" "$MODELS_DIR" 2>/dev/null || true
    return 0
}

# Remove one path, with sudo only where it is actually needed.
rm_path() {
    local target="$1"
    [ -e "$target" ] || return 0
    if [ -w "$(dirname "$target")" ]; then rm -rf "$target"; else $SUDO rm -rf "$target"; fi
    muted "removed $target"
}

uninstall() {
    banner
    printf '\n%s==>%s %sRemoving Crucible%s\n\n' "$C_CYN" "$C_RESET" "$C_BOLD" "$C_RESET"

    # Every prefix an install could have used, each named once. PREFIX is
    # normally one of the other two, and asking the same directory twice would
    # report its own leftovers as a second install.
    local prefixes=() p seen
    for p in "$PREFIX" /usr/local "$HOME/.local"; do
        [ -n "$p" ] || continue
        for seen in ${prefixes[@]+"${prefixes[@]}"}; do
            [ "$seen" = "$p" ] && continue 2
        done
        prefixes+=("$p")
    done

    local cfg="$CONFIG_DIR"
    local dat="$DATA_DIR"
    local cache="${XDG_CACHE_HOME:-$HOME/.cache}/crucible"

    # The binary's own uninstaller is the real one: it knows the prefix it was
    # installed into, which may be neither of the two guessed at above, and it
    # removes the same set of things this does.
    #
    # It is run for every prefix that has one, not just the first. A machine
    # with a system install and a user install had the second one left behind,
    # still on PATH, pointing at libraries the first pass had deleted.
    local exe rc=0 status
    local handled=()
    for p in "${prefixes[@]}"; do
        exe="$p/bin/crucible"
        [ -x "$exe" ] || continue
        status=0
        if [ "$ASSUME_YES" = 1 ]; then
            "$exe" --uninstall --yes || status=$?
        elif have_tty; then
            # stdin has to be the terminal. Under the documented one-liner --
            # `curl ... | bash -s -- --uninstall` -- stdin is the installer
            # script still being read, so the binary's question was answered
            # with a line of shell and taken as "no", and it reported "Done."
            # having removed nothing at all.
            "$exe" --uninstall < /dev/tty || status=$?
        else
            # Nothing to ask on. Uninstalling was the explicit request and the
            # answer defaults to yes, which is what confirm() does in the same
            # situation.
            "$exe" --uninstall --yes || status=$?
        fi
        # A run that succeeded and left its own binary on disk is one where the
        # answer was no. Nothing else here is ours to remove -- and without
        # this the sweep below would go on to delete the whole prefix, which
        # is precisely what was just declined.
        if [ "$status" -eq 0 ] && [ -e "$exe" ]; then
            info "nothing was removed"
            exit 0
        fi
        # Only a run that succeeded speaks for its prefix. One that could not
        # ask its question has settled nothing, and the sweep below has to
        # finish the job rather than treat the binary's presence as consent.
        if [ "$status" -eq 0 ]; then
            handled+=("$p")
        else
            rc="$status"
        fi
    done

    # Whatever the binary could not speak for: a prefix with no binary in it,
    # and a prefix whose binary was there but could not finish. Where its own
    # uninstaller did run it has already taken everything below, so this only
    # covers what it could not reach.
    local sweep=() name entry
    for p in "${prefixes[@]}"; do
        local settled=0
        for seen in ${handled[@]+"${handled[@]}"}; do
            [ "$seen" = "$p" ] && { settled=1; break; }
        done
        [ "$settled" = 1 ] && continue
        for name in crucible crucible-gui crucible-routebench; do
            [ -e "$p/bin/$name" ] && sweep+=("$p/bin/$name")
        done
        # llama.cpp's shared libraries, the application-menu entry and its
        # icon. Left behind, the menu goes on offering a Crucible that is no
        # longer installed.
        [ -d "$p/lib/crucible" ] && sweep+=("$p/lib/crucible")
        for entry in "$p/share/applications/crucible.desktop" \
                     "$p/share/icons/hicolor/scalable/apps/crucible.svg"; do
            [ -e "$entry" ] && sweep+=("$entry")
        done
    done

    # The user's own files, and the installer's source checkout inside them.
    # Only ours to remove when no binary spoke for its prefix -- one that did
    # has already asked about these and acted on the answer.
    if [ "${#handled[@]}" -eq 0 ]; then
        for entry in "$cfg" "$dat" "$cache"; do
            [ -e "$entry" ] && sweep+=("$entry")
        done
    fi

    # One question, and it names everything. It used to be three -- programs,
    # then configuration, then models and history -- which made a full removal
    # a thing you had to agree to three times and a partial one the easiest
    # outcome to get by accident. "Uninstall" means uninstall.
    if [ "${#sweep[@]}" -gt 0 ]; then
        info "this will remove:"
        for entry in "${sweep[@]}"; do
            muted "$entry"
        done
        printf '\n'
        if ! confirm "Remove Crucible and everything above?" y; then
            info "nothing was removed"
            exit 0
        fi
        for entry in "${sweep[@]}"; do
            rm_path "$entry"
        done
    elif [ "${#handled[@]}" -eq 0 ]; then
        info "no installed crucible found"
    fi

    # Say what survived rather than claiming success over the top of it.
    local left=0 leftover
    for p in "${prefixes[@]}"; do
        for name in crucible crucible-gui crucible-routebench; do
            [ -e "$p/bin/$name" ] && { warn "still present: $p/bin/$name"; left=1; }
        done
        [ -e "$p/lib/crucible" ] && { warn "still present: $p/lib/crucible"; left=1; }
        for entry in "$p/share/applications/crucible.desktop" \
                     "$p/share/icons/hicolor/scalable/apps/crucible.svg"; do
            [ -e "$entry" ] && { warn "still present: $entry"; left=1; }
        done
    done
    for leftover in "$cfg" "$dat" "$cache"; do
        [ -e "$leftover" ] && { warn "still present: $leftover"; left=1; }
    done
    [ "$left" = 1 ] && exit 1
    [ "$rc" -eq 0 ] || exit "$rc"
    ok "done"
    exit 0
}

# --------------------------------------------------------------------------
# PATH advice
# --------------------------------------------------------------------------
path_advice() {
    case ":$PATH:" in
        *":$PREFIX/bin:"*) return ;;
    esac

    printf '\n'
    warn "$PREFIX/bin is not on your PATH."
    # Parameter expansion rather than basename: no external command, so this
    # still works if PATH is minimal or unusual.
    case "${SHELL##*/}" in
        fish)
            info "add it with:"
            printf '        %sfish_add_path %s/bin%s\n' "$C_BOLD" "$PREFIX" "$C_RESET" ;;
        zsh)
            info "add it with:"
            printf '        %secho '\''export PATH="%s/bin:$PATH"'\'' >> ~/.zshrc%s\n' "$C_BOLD" "$PREFIX" "$C_RESET" ;;
        *)
            info "add it with:"
            printf '        %secho '\''export PATH="%s/bin:$PATH"'\'' >> ~/.bashrc%s\n' "$C_BOLD" "$PREFIX" "$C_RESET" ;;
    esac
}

# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------
# Report what an install would do, changing nothing. Deliberately never asks
# for sudo, so it is safe to run anywhere -- including on someone else's box
# before deciding whether to install at all.
run_check() {
    local saved_deps="$INSTALL_DEPS"
    INSTALL_DEPS=0          # suppresses the sudo pre-authorisation
    banner
    printf '%s==>%s %sDry run -- nothing will be changed%s\n\n' \
        "$C_CYN" "$C_RESET" "$C_BOLD" "$C_RESET"

    detect_platform
    INSTALL_DEPS="$saved_deps"

    printf '\n'
    if cmake_version_ok cmake; then
        ok "$(cmake --version | head -1)"
    else
        info "cmake is missing or older than ${CMAKE_MIN_MAJOR}.${CMAKE_MIN_MINOR};"
        info "CMake ${CMAKE_BOOTSTRAP_VERSION} would be downloaded to ${XDG_CACHE_HOME:-$HOME/.cache}/crucible"
    fi

    printf '\n%swould install:%s\n' "$C_BOLD" "$C_RESET"
    if [ "$INSTALL_DEPS" = 1 ]; then
        resolve_packages
        info "toolchain  : ${PKGS_BASE[*]:-none}"
        info "desktop    : ${PKGS_GUI[*]:-none needed}"
    else
        muted "(package installation disabled with --no-deps)"
    fi

    printf '\n%swould build and install:%s\n' "$C_BOLD" "$C_RESET"
    info "binary     : $PREFIX/bin/crucible"
    # The same question the real install asks, so a dry run cannot promise a
    # desktop app the install would then step down from. It is only the whole
    # answer when the headers are already here: --check installs nothing, so
    # packages the install would have added first are not counted.
    local gui_before="$WITH_GUI"
    verify_gui_prerequisites
    if [ "$WITH_GUI" = "1" ]; then
        info "desktop app: $PREFIX/bin/crucible-gui"
    elif [ "$gui_before" = "0" ]; then
        muted "desktop app: skipped (--no-gui)"
    else
        muted "desktop app: only if the packages above supply the headers"
    fi
    WITH_GUI="$gui_before"
    info "libraries  : $PREFIX/lib/crucible"
    info "config     : $CONFIG_DIR"
    info "models     : $MODELS_DIR"
    muted "No compute runtime is installed. Crucible builds one on demand from"
    muted "its settings screen, on the machine that will run it."
    printf '\n'
    exit 0
}

main() {
    choose_prefix
    [ "$DO_CHECK" = 1 ] && run_check
    banner
    [ "$DO_UNINSTALL" = 1 ] && uninstall

    progress_begin

    step "Checking your system"
    phase 100 "detecting the platform"
    detect_platform
    phase_end

    if [ "$INSTALL_DEPS" = 1 ]; then
        step "Installing dependencies"
        install_dependencies
    else
        step "Skipping dependency installation (--no-deps)"
    fi

    step "Checking CMake"
    phase 100 "checking the CMake version"
    ensure_cmake
    phase_end

    step "Getting the source"
    phase 100 "fetching $REPO_URL"
    command -v git >/dev/null 2>&1 || die "git is required but not installed"
    locate_source
    phase_end

    step "Building Crucible"
    # Last word on the desktop app, and it belongs here rather than in the
    # dependency step: this runs whether or not packages were installed, so
    # --no-deps and a machine that already had the headers both get a straight
    # answer instead of an inherited guess.
    verify_gui_prerequisites
    build_and_install

    # The directories the program keeps its files in, so the first run opens
    # onto somewhere that exists rather than reporting a path that does not.
    make_directories

    progress_end

    printf '\n%s%s  Crucible is installed.%s\n\n' "$C_GRN" "$C_BOLD" "$C_RESET"
    printf '    crucible      %s\n' "$PREFIX/bin/crucible"
    # WITH_GUI is whatever survived the dependency step, so this reports what
    # was actually built rather than what was asked for.
    if [ "$WITH_GUI" = "1" ]; then
        printf '    crucible-gui  %s\n' "$PREFIX/bin/crucible-gui"
    else
        printf '    crucible-gui  %snot installed%s\n' "$C_YEL" "$C_RESET"
    fi
    printf '    config        %s\n' "$CONFIG_DIR"
    printf '    models        %s\n' "$MODELS_DIR"

    path_advice

    printf '\n  To remove it:  %scurl -fsSL %s | bash -s -- --uninstall%s\n\n' \
        "$C_BOLD" "$RAW_URL" "$C_RESET"
}

# Sourcing with CRUCIBLE_INSTALL_LIB=1 loads the helpers without running anything,
# which is how tests/test_install.sh checks the version and backend logic.
if [ -z "${CRUCIBLE_INSTALL_LIB:-}" ]; then
    main "$@"
fi
