# Changelog

Crucible is versioned `MAJOR.MINOR.PATCH`. A release is a `v`-prefixed tag on
GitHub, which builds the installers for macOS, Windows and Linux and attaches
them to the release. Running Crucible checks for a newer tag once a day and says
so in **Settings → About**; updating is the same one-line installer that put it
there, and it keeps your config, models and history.

## 0.5.5 — 2026-09-27

A pass over the whole window: less text, more room, and the small things that
were wrong every day.

**It says less.** Every panel had a paragraph under its heading explaining what
the panel was for, and none of them were read twice. They are gone or down to a
line. The controls and their names carry it now, and what genuinely needs
saying -- what a folder is being trusted with -- is two lines in the one dialog
that asks for consent.

**It is easier to read.** The reading measure came down from 110 characters to
90, which is where a paragraph and an eighty-column diff both sit comfortably.
Panels round their corners, the spacing is looser, and the scrollbars are thin
and dim instead of being the brightest thing on a near-black screen.

**The top bar is a name, not a path.** It was `Project: /home/you/code/…/thing`
plus a Change project button, taking half the bar to say what a window title
says in a word. It is now the folder's name, and clicking it is how you change
project. The path is the tooltip.

**An empty chat suggests something to type.** Three chips that fill the box
rather than sending, because the first thing anybody does with a suggestion is
edit it.

**Glitches.** The box you type in could grow a scrollbar of its own and swallow
the wheel -- it cannot now, at any window size or font scale. A long error in
the folder picker pushed Open and Cancel off the bottom of the dialog, leaving
no way to answer it but Escape. Model names no longer end mid-glyph at the edge
of the sidebar, and they no longer carry a `.gguf` that pushed the name out.
Scrolling back through a conversation that is still streaming now offers a
Jump to latest rather than either yanking you to the end or stranding you.

## 0.5.1 — 2026-09-22

**It opens on nothing.** Crucible used to pick a directory for you -- the
shell's working directory from a terminal, the most recent project from the
application menu -- and both were guesses. The menu one guessed wrong every
time the launcher handed over `/` or the home directory, and a window would
open asking to be trusted with somewhere nobody meant to work. Now the top bar
says **No Project**, the button beside it says **Open Project**, and the folder
question is asked when a folder is actually chosen. Chat, Cook and History wait
for one rather than pretending.

**Less text.** The runtimes page carried a sentence per backend explaining what
CUDA is for; it now lists the llama.cpp version it is built against and the tool
it needs, which is what you cannot work out by looking. The Create tab's Target
step lost the explanations under the method and the format -- the number beside
each method, what it needs against what the card has, is what decides between
them.

**Darker.** The panels were a step lighter at every level than they should have
been, which read as gray rather than as dark. The whole ladder moved down to
near-black.

## 0.5.0 — 2026-09-21

The release that makes Crucible a program you install rather than a checkout you
build.

**Create.** A new tab that fine-tunes an expert instead of asking you to find
one: a base model and a dataset browsed from Huggingface inside the app or
brought from your disk, QLoRA or LoRA, an estimate of what will fit on your card
before the run rather than forty minutes into it, and a seat on the roster at the
end. The recipe, the browsing and the estimate work today; the training run,
downloads, test bench and export are being built.

**One program.** The terminal interface is gone and `crucible` is the window.
FTXUI went with it; `crucible-gui` is no longer installed, and an uninstall
sweeps away one left over from an older install.

**Something to click.** The installers now leave an application, not just a
command: a bundle in Applications on macOS, a menu entry on Linux, Start Menu and
Desktop shortcuts on Windows — all pinnable. There are also downloads that need no
compiler at all: a `.dmg`, a setup `.exe` and an AppImage, built by CI from a tag.

**Knowing when to update.** One request a day to GitHub's releases API for a
version number, cached, off with a checkbox, and shown as a dot on the gear.
Nothing about the machine goes with it.

**Also:** display scaling follows the monitor the window is on, the Cook screen
carries the same token and context readout as Chat, the expert picker offers
models Crucible made alongside the ones on the disk, and the README is a page
rather than a book.
