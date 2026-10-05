# Changelog

Crucible is versioned `MAJOR.MINOR.PATCH`. A release is a `v`-prefixed tag on
GitHub, which builds the installers for macOS, Windows and Linux and attaches
them to the release. Running Crucible checks for a newer tag once a day and says
so in **Settings → About**; updating is the same one-line installer that put it
there, and it keeps your config, models and history.

## Unreleased

**An expert can be a model that is somewhere else.** A seat is a name, a
description and a model, and the model no longer has to be a file: it can be
Claude through Anthropic's API, or anything that speaks the OpenAI one --
OpenAI, Gemini, DeepSeek, Moonshot's Kimi, Cloudflare Workers AI, OpenRouter,
Groq, or a llama.cpp, Ollama or LM Studio server on your own network.

- **Settings → Providers** adds one: an address, a key, and the models it
  offers, which it can be asked for. A key typed there is written to the config
  file, and the screen says so; `env:NAME` reads it from the environment
  instead.
- A provider's model is picked for a seat like any other, and can be the
  **default expert** -- the one that catches what the local ones could not
  place.
- **Nothing is sent until you set that up**, and a seat that leaves the machine
  is marked as one. The delegator stays local: it is asked for a probability
  per expert, which no provider's API gives.
- Reasoning arrives in the same "thinking" disclosure, token counts are the
  provider's own, and Stop interrupts a request rather than waiting for it.
- When Claude declines a request the turn says so, and what had streamed is
  not left on screen as an answer. Against Anthropic's own API, a declined
  request may be answered by another of its models; the transcript says when.
- `/physics why is the sky blue` sends a prompt straight to that expert.

**An expert no longer has to be trained.** **New expert** -- on Settings →
Experts -- takes a name, a description and a model you already have. Create is
still how you fine-tune one; this is how you use one as it is. **Eject** takes
a seat off the roster.

**The window stops freezing.** Everything the page asks for used to be
answered on the thread that draws, so opening Settings waited for a disk scan,
a hardware probe and a Python interpreter. The slow questions are answered on
threads of their own now, the state is pushed to the page thirty times a
second rather than once per token, and a finished turn is drawn once rather
than on every push.

**Browse opens the system's own file dialog** -- its bookmarks, its mounted
drives, the last place you were -- for a project, the models directory, a model
for a seat, and every "or from this machine" in Create. Crucible's own picker
is still there for a desktop with no dialog to give.

**Where things are, at a glance.**
- The project's path is in the top bar, beside the button that changes it,
  and the window's title is the project's name.
- A model that is loading shows a ring that fills, with the percentage in it,
  on the line of its name -- the delegator's as well as an expert's.
- The status line is at the top of the side menu, and a line runs from the
  delegator to whichever seat has the turn.

**The rest of what the old window did.**
- **Settings has no Save buttons.** Every control applies as it is changed.
- **Create is a six-step dialog** -- Name, Base model, Data, Tools, Target,
  Review -- with what a choice would cost beside the choice: the memory a
  method needs against what the machine has, the size of what comes out. A
  run shows its loss as a curve. A run that finishes is written back to its
  recipe whichever view you are on.
- **Chat** says what is missing before a first answer -- no project, no
  runtime, no model -- with the button that fixes it, and offers somewhere to
  start when nothing is. The box is several lines and grows. What an expert
  did is shown where it did it rather than folded away. Scrolling up stays
  where you put it, with **Jump to latest** to come back.
- **Cook** shows the goal, the state, who has had the work, and what changed
  on disk -- and says so in red when a cook that claims to have finished
  changed nothing.
- **Hardware** names its modes and says why a setting cannot work on this
  machine rather than letting it be set. **Runtimes** lists what each is made
  of. **Training** can be removed. **About** says what version this is, where
  everything is kept, and exactly what leaves the machine.
- Notices -- "opened demo", "Physics has joined the experts" -- reach the
  transcript; the list they were written to was read by nothing.
- Ctrl+1 to 4 switch views and Ctrl+, opens Settings. The gear is a toggle.

**Underneath.** The page is a stylesheet and a script per view rather than
one file, assembled at build time by CMake alone. Each view is a function from
the state to markup, which is what lets the tests draw every screen and dialog
with no browser. The surface behind it is a table of methods split by subject,
and one curl wrapper replaced four -- which also took a search key off a
command line where any process on the machine could read it.

## 0.7.1 — 2026-10-04

**The interface reaches parity with the window it replaced.** The webview
shipped in 0.7.0 with four things missing and two glitches; this is them.

- **The side menu folds and resizes.** A button in the top bar, and the edge
  is a handle. Closed is a width rather than a mode, so the button and the
  drag do the same thing, and below a threshold it becomes a rail of dots
  rather than nothing. **Eject** and **Manage experts** are back on it.
- **A turn can be stopped, asked again, or deleted**, from controls that
  appear when you hover it. While something is running the only offer is
  Stop, and only on the turn that is running.
- **The token tally is always on screen** and belongs to the composer rather
  than sitting below it. It used to appear only after the first reply, and
  the gap it appeared in showed the page's background through it.
- **A cook runs until it is done or until you stop it.** The minutes box is
  gone -- it asked you to commit to a number before you knew what the work
  was. **Stop and finish** makes the wrap-up pass that leaves the project
  working; **Stop now** does not.
- **The Create wizard asks in the order you answer in:** search Huggingface,
  then "or from this machine", for the base model and the data both. The data
  box has its own search now.
- **Settings** is a gear again, and has the controls it was missing: Browse
  for the models directory, keep-the-delegator-loaded, a confidence floor;
  New expert, Rescan models and a default expert; sliders beside the number
  boxes for temperature, top P, top K, min P and repeat penalty, and the
  context-truncation choice; the order the GPUs are filled in; runtimes that
  open to show what they were built from, and can be deleted; a command
  timeout slider, and a web-search section that reveals its settings when it
  is switched on.

**The chat reads like a transcript again.** What you asked sits in a card
with the flame down its edge rather than as a line of body text under the
word "you". Above each answer is who gave it and how the delegator got there
-- `Mathematics · 67% · router model · swapped in 100.4s` -- and under it what
it cost. A fenced block has a header saying the language and how many lines,
a copy button, and a numbered gutter beside the code. Inline code is a name
rather than more prose. Turns are separated by a rule.

**A file an expert wants to write is shown as the file.** The same block the
reply's code is drawn in, tinted green for a new file, with **Allow** and
**Deny** under it. A change to an existing file shows the lines that move
rather than the whole thing, because the rest of the file is not what is
being decided.

**Fixed:** opening **Create** or **Settings** froze the window. Both awaited
everything the view needed before drawing it, on the thread that draws, and
three of those calls touch the machine. The view is drawn first now and each
answer arrives into it. **Browse** on the models directory appeared to do
nothing: it wrote the chosen path to the input, and the redraw that followed
rebuilt the page from the configuration and wiped it. Deleting a turn changed
the transcript without waking the page, so it stayed on screen. "Keep the delegator in memory" on the Hardware page was
reading a setting that does not exist at that path and never did anything; it
is on General now, where the setting actually lives.

## 0.7.0 — 2026-10-04

**The window is the platform's own webview.** Dear ImGui, GLFW and OpenGL are
gone. Crucible draws itself in WebView2 on Windows, WKWebView on macOS and
WebKitGTK on Linux, on a page compiled into the binary -- no server, no port,
no browser tab, and nothing fetched at runtime. There is no `--ui` flag,
because there is nothing left to choose between. On Linux this adds one
dependency, `libwebkit2gtk-4.1`, which the installer checks for and names.

Everything the window did, the page does: chat, cook, create, history, and all
eight settings pages. Replies stretch the full width of the transcript with a
margin rather than sitting in a narrow column, and code blocks are colored.

**Four things that quietly stopped happening.** The old interface did them
once a frame and a page has no frames: conversations were not written to
history, the delegator's routing examples were not folded back into the
config, the daily update check never ran, and a compute runtime could not be
built from the settings screen. All four are back, on the one thread that
corresponds to a frame.

**Trying a fine-tune before keeping it.** A trained model opens a window that
really runs it, on a seat that exists only while the window is open and is
never written to the config. **Finish** keeps it and gives it a seat the
delegator can route to; **Edit** goes back to the fine-tune parameters.

**A folder path can be typed or pasted.** The picker's path line is an input
now. Clicking down from the home folder is fine for a folder you are looking
for and tedious for one you already know.

**Fixed:** text interpolated into an HTML attribute escaped only `<` and `>`,
so a quote in a model name or a Huggingface search result could break out of
the attribute it was in. CMake keywords were never highlighted, because the
case-folding uppercased a word and compared it against a lowercase list.
Counts read "1 turns".

**Internal:** `util/markdown`, `util/syntax`, `util/code_lines` and
`util/display_scale` are deleted -- about 2,300 lines that existed to serve
the ImGui renderer. The first three are JavaScript in `ui/render.js`, which is
a separate file from the page so that a test can run it without a document;
`tests/test_ui_js.cpp` evaluates it in JavaScriptCore, the engine WebKitGTK
gives the webview, so their cases carried across rather than being dropped.

**Known issues.** The interface is new and this release does not pretend
otherwise:

- The sidebar does not collapse yet.
- A prompt or a reply cannot be canceled, asked again or deleted from the
  transcript. The engine can do all three; the page has no buttons for them.
- Opening **Create** or **Settings** stalls for about a second the first time.
  Each fetches what it needs on the way in, on the thread that draws.
- The composer can shift upward, and because it is drawn in the background
  color the move reads as the window jumping rather than as a control moving.

## 0.6.5 — 2026-09-28


**Create is a list of experts, not a form.** The tab used to open on step one
of eight whether or not you had come to fill anything in, with the models you
had already made listed in a rail beside it. It opens on them now: what has
been made, what each one is for, what it was built from, and where it got to.
Making another is a **New expert** button that walks the same questions through
a wizard and gets out of the way when the run starts.

**A recipe has a life now, and the list is sorted by it.** Draft, training,
ready to test, finished. Starting a run closes the wizard and puts the model in
the list as training; clicking it opens its own page, with its specs and its
progress. Finishing is what puts a model on the roster -- so a half-trained
adapter no longer becomes routable because a file with its name turned up on
disk, which is what the old "has it got a path" test amounted to. Recipes
written by 0.6.0 and earlier are read as ready to test when they point at a
file and as drafts when they do not.

**A test window, and it really runs the model.** Ready-to-test opens a window
that seats the candidate and talks to it through the same engine Chat uses --
one model host in the process, so the seat is temporary and never reaches the
config file. **Finish** keeps it and gives it a seat on the roster, with its
description as what the delegator routes on; **Edit** goes back to the
fine-tune parameters.

**Crucible trains the model now, and the installer sets that up.** The gap was
a Python environment: adapter training is peft, bitsandbytes and a torch built
for the card, none of which a C++ program has any business reimplementing. So
the installer builds one -- a private virtual environment in Crucible's own
data folder, made from whatever Python the machine already has, removable by
deleting a folder. It cannot break a system package and cannot be broken by
one. Start training and the run actually runs: the page shows the phase, the
step count, the loss curve, what card it is on and how long is left, and Stop
works within a step.

It is several gigabytes -- about 3 GB down the wire and 7 GB on disk for the
CUDA stack -- so the size is printed before it starts, `--no-trainer` skips
it, and `crucible --install-trainer` does it afterwards, which is also the
repair command for a machine that had no Python at install time.
**Settings -> Training** shows what is installed, what it can train on, and
removes it.

Worth knowing where that goes, because it is not Crucible: about three fifths
is NVIDIA's own CUDA libraries, which any PyTorch install pulls in, and
PyTorch itself is another 1.6 GB. pip is told not to keep its download cache,
which would otherwise leave 2.7 GB in `~/.cache/pip` that removing the
environment would not reclaim -- the folder this promises to be removable by
deleting is now actually all of it.

**Which PyTorch, decided by the cards rather than pinned.** A torch wheel
carries kernels for a fixed list of architectures, and a card newer than the
list is not slow, it is unusable -- while `import torch` succeeds and
`cuda.is_available()` still says true. Pinning CUDA 12.4 cost an RTX 5060 Ti
on the machine this was built on: two cards worked, the third sat idle, and
nothing said so. The index now follows the newest compute capability present,
and the environment reports any card its torch has no kernels for.

A run is a child of the window that started it and ends with it, as a runtime
build does. The page says that rather than letting somebody close a window on
an hour of training.

Export: a fine-tune comes out as a GGUF at F16 or Q8_0, or as a Huggingface
model directory when llama.cpp's converter is not on the machine. The k-quants
need `llama-quantize`, which Crucible does not build yet, and the run says so
instead of silently producing something the recipe did not ask for.

Attaching a file trained elsewhere still works, and is still the right answer
for a model fine-tuned with unsloth or axolotl.

Smaller things: a learning rate reads as `1.0e-05` rather than `10.0e-06`; long
model paths are cut from the middle instead of running off the window; a
resizable window's corner grip is no longer ImGui's default blue.

## 0.6.0 — 2026-09-27


**Runtimes can arrive already built.** Compiling a backend on the machine that
will run it stays the fallback and stays correct, but for CUDA it cost four and
a half minutes and a multi-gigabyte toolkit that had to be installed first --
and that toolkit, not the minutes, is what stopped people. The release now
carries the same modules built from the same llama.cpp tag, and the app asks for
one by a name holding everything that has to match. The shipped CUDA module
links cudart statically, so it needs a driver and nothing else -- a 580-series
driver or newer, since it is built with CUDA 13; anything older compiles
locally as before. Install is never
disabled now: it used to switch off on a missing `nvcc`, which shut out exactly
the people a download is for.

**Runtimes list what they are.** Each one folds open to its modules -- the file,
its size, and which of them ggml actually loads. A CPU runtime is fourteen
modules, one per processor feature level, and runs one; nothing said so before.
Under the list is where they live, what they cost together, and a warning for
any built against a different llama.cpp.

**Fetching the source needs no git.** It takes the tarball with curl and tar,
which every platform has, and falls back to cloning. Anyone who installed from
a `.dmg`, a setup `.exe` or an AppImage used to hit "git is needed" on their
first GPU backend. Missing cmake is now reported before the button rather than
five minutes into a log.

**Closing the side menu keeps the dots.** It used to close completely, taking
the one picture of what the program is doing at the moment you most want it --
you close it to give a long cook the width. Closed is now a rail of the same
dots, each naming itself on hover.

**Keys.** Ctrl+1..4 for the views, Ctrl+, for Settings, and the open project in
the window title.

**A real icon.** The mark was shapes Crucible generated from its own control
points. It is a drawing now -- an ASCII flame -- and one script turns that file
into all four the program needs: the application icon, the Windows `.ico`, the
Linux menu icon, and the mark compiled into the binary. Rounded like the icons
either side of it in a dock, and the window hands it to the window manager, so
the taskbar and the alt-tab list stop showing a placeholder.

**Less to read.** The slash-command completion and its nine tests went with the
terminal interface they belonged to, along with a path elider nothing had
called since the top bar stopped printing paths. The icons moved into
`packaging/icons/`, the desktop entry in with the rest of the Linux packaging,
and the ignore list grew to cover what the packaging scripts leave behind.

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
