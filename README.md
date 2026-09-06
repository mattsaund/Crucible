```
   ⠀⠀⠀⠀⠀⠀⢱⣆⠀⠀⠀⠀⠀⠀
   ⠀⠀⠀⠀⠀⠀⠈⣿⣷⡀⠀⠀⠀⠀
   ⠀⠀⠀⠀⠀⠀⢸⣿⣿⣷⣧⠀⠀⠀
   ⠀⠀⠀⠀⡀⢠⣿⡟⣿⣿⣿⡇⠀⠀
   ⠀⠀⠀⠀⣳⣼⣿⡏⢸⣿⣿⣿⢀⠀   C R U C I B L E
   ⠀⠀⠀⣰⣿⣿⡿⠁⢸⣿⣿⡟⣼⡆   a local LLM engine that delegates.
   ⢰⢀⣾⣿⣿⠟⠀⠀⣾⢿⣿⣿⣿⣿
   ⢸⣿⣿⣿⡏⠀⠀⠀⠃⠸⣿⣿⣿⡿
   ⢳⣿⣿⣿⠀⠀⠀⠀⠀⠀⢹⣿⡿⡁
   ⠀⠹⣿⣿⡄⠀⠀⠀⠀⠀⢠⣿⡞⠁
   ⠀⠀⠈⠛⢿⣄⠀⠀⠀⣠⠞⠋⠀⠀
   ⠀⠀⠀⠀⠀⠀⠉⠀⠀⠀⠀⠀⠀⠀
```

**A local AI engine that delegates, and then keeps working.**

Two things, in one program.

**It delegates.** You bring a set of small, specialised models. A tiny
*delegator* reads your prompt, names the expert it belongs to, and that expert
is loaded just in time to answer it. One model is resident at a time, so the
memory you need is the largest expert rather than the sum of them.

**It cooks.** Give it a goal and a length of time instead of a question, and it
works on the project on disk: reads files, changes them, runs them, reads the
failure, changes them again, for as long as you gave it. When the time is up it
makes a finishing pass to leave things in a state that runs, and writes down
what it did.

Both faces are the same program. `crucible` is a terminal application;
`crucible-gui` is a desktop one. They share an engine, a config file and a
history — an expert added in one is there in the other.

---

## Why

A single local model has to fit on your hardware, so it is always smaller than
you would like. Ten specialised models plus a delegator do not: only one is
resident at a time, so ten 30B experts occupy the space of one and behave more
like a 300B model than any of them could alone. The cost is a load per swap,
which is the trade the whole design is built around.

And a question is not the only shape work comes in. Most of what you want from a
model on your own machine is not one answer — it is an afternoon of small
changes to something that already exists. That is what a cook is.

---

## The experts

No experts ship. A new install has an empty roster, and every seat on it is one
you made — with `/newexpert`, the desktop app's Experts page, or by writing an
entry in the config file. A filled-in roster looks like this:

```
                                          ◇ Mathematics
                    ⠀⠀⠀⠸⣦⠀⠀⠀⠀             ◇ Programming
                    ⠀⠀⠀⢰⣿⣷⡄⠀⠀             ◆ Physics ────────┐
                    ⠀⠀⣄⣿⠟⣿⣿⠀⠀  Crucible   ◇ Chemistry       │
                    ⠀⣰⣿⡟⢁⣿⣿⣿⡀         ◆   ◇ Biology         │
                    ⣾⣿⡏⠀⠘⢹⣿⣿⠇             ◇ Engineering ────┘
                    ⢿⣿⠀⠀⠀⠀⢻⡯⠀             ◇ Philosophy
                    ⠈⠛⠧⣀⠀⠠⠛⠁⠀             ◇ Sociology
                                          ◇ Language
```

Until you add one the panel simply says so, and the delegator has nobody to
route to.

The flame says what the machine is doing — an ember when idle, burning steadily
while a model is loading, the full plume while tokens are coming out, and smoke
over a cold foot when something has failed. It is the mark at the top of this
file, resampled onto a character grid; a terminal with no braille in its font
gets the same silhouette in hashes instead. The line joins Crucible to whichever
expert the delegation chose.

### Making your own

```
/newexpert
```

opens two boxes: a name, and what the expert is trained in. That is all you are
asked for. The id, the four-character chip, and the keyword set the model-free
router scores with are derived from what you typed. The worked examples the
delegator routes on are written **by the delegator itself** — two example
questions per seat is worth seven points of routing accuracy on the benchmark
(89% to 96%), and it is the one input a description cannot stand in for.

```
/ejectexpert chemistry
```

removes one. Nothing is ever put back: eject a seat and it stays ejected.

A new expert is routable the moment it exists. Point it at a GGUF from
`/settings` and it starts answering; leave it empty and prompts routed to it are
redirected to whichever seat you nominated as the **default expert** — or, if
you nominated none, to any filled one, with the transcript saying so.

There is no built-in catch-all. Crucible used to ship a tenth seat called
Fallback that the delegator was forbidden from naming; with a roster you own,
"a general-purpose model for anything that does not fit" is just an expert you
add and then nominate in `/settings`.

---

## Cooking

```
/cook fix the failing tests and tidy up the parser
/cook 30m make the CLI take a --json flag
/stop
```

A cook is a goal. It runs until the work is done or until you `/stop` it.

The expert works one action at a time, and you watch it happen:

```
cook ▸ fix the bug in calc.py so that test_calc.py passes
     working  ·  round 2  ·  4 minutes

     list    listed . (2 entries)
     read    read calc.py
     note    the add function subtracts instead of adding
     write   updated calc.py (8 lines)
     run     $ python3 -m pytest -q  -- exit 0
     done    fixed the sign error and confirmed the test passes

     changed  calc.py
```

It can stop and ask you something, and the next thing you type answers it:

```
   ? Should the parser reject a trailing comma, or accept it silently?
     type an answer and press enter
```

**One cook, several experts.** The expert that writes the code is not the one
that should write the documentation, so a cook does not hold one for the whole
hour. Finishing a piece of work prompts for the next one, and that line goes
back through the delegator:

```
     done     fixed the sign error and confirmed the test passes
     handoff  document the parser's public functions
     note     Programming handed over to Language
     write    updated README.md  +18
```

The previous model is freed before the next is loaded, so the peak is still the
larger of the two and never their sum. An expert can also hand over mid-piece
with `HANDOFF: <the next work>`. If nobody else can take it, the one already
loaded carries on — a worse specialist finishing the job beats no job.

**`/stop` is not cancel.** It stops the cook taking new work and runs a
finishing pass whose only job is to leave the project in a state that runs —
finish a half-made edit, repair what broke, check it starts. `Ctrl-C` is still
there for "stop now".

`/cooks` lists what every past cook in this project changed and how long it
took. The journal is written as the cook runs, not at the end, so a cook killed
at minute fifty can still tell you what it did.

> A finished cook that wrote no files says so, in red, under its own summary.
> The summary is the expert's account of itself; the journal is the fact. They
> disagree more often than you would like.

### The workshop

Cooking needs the workshop, and the workshop is **off until you turn it on**, in
`/settings` under TOOLS. It is what lets an expert act on a project rather than
describe it:

| | |
|---|---|
| `LIST:` | what is in a directory |
| `READ:` | a file, with line numbers |
| `WRITE:` | replace a file |
| `RUN:` | run a command, starting in the project root |
| `SEARCH:` | look something up (needs web search on) |
| `ASK:` | ask you something |
| `HANDOFF:` | this piece is done and the next needs a different expert |
| `NOTE:` `DONE:` | record what it is doing; close a piece of work |

Three things are load-bearing and none of them is the tool list.

**Every file an expert reads or writes is resolved inside the project root, and
anything that escapes is refused** — absolute paths, `..`, and symlinks, which
are resolved *before* the check rather than after. Containment compares path
components rather than strings, because `/home/me/proj` is a text prefix of
`/home/me/project-two` and is not a parent of it. An expert that can write
outside the project is not a coding assistant, it is a remote shell, and the
difference has to be structural.

**`RUN` is the exception, and it is worth being exact about.** A command starts
with the project root as its working directory and that is the whole of the
containment: a shell can `cd` anywhere, read anything you can read, and reach
the network. That is not theoretical — an expert on a test cook wrote
`RUN: cd /tmp/… && grep -r add src/` and it ran, exactly as a shell should.

It is not fixed with a blocklist. `cd`, `..`, `$(…)`, an absolute path and
`find /` are five ways to do one thing and there are fifty more; refusing a
subset while claiming confinement would be worse than saying plainly what this
is. Real confinement means a sandbox — bubblewrap, Landlock, seatbelt — which is
per-platform and is not here yet.

**So it is two switches, not one.** Letting a model edit a project you already
trusted and letting it run commands *as you* are different decisions, and only
the first is bounded by a directory. `RUN` can be off while the rest is on. Both
sit on top of the folder-trust prompt Crucible asks on first use in a directory.

**It is a text protocol.** llama.cpp applies a chat template, it does not
negotiate a tool schema, and Crucible cannot know which model is in the seat. A
convention every model can follow beats one only the tool-trained models can.

---

## The desktop app

```sh
crucible-gui                # installed by default; --no-gui skips it
```

On Linux the installer also adds it to your application menu, so it can be
started by clicking an icon like any other desktop program. Launched that way
there is no terminal to ask the folder-trust question on, so the window asks it
instead, and it opens in your last project rather than in whatever directory the
launcher happened to be in.

Building it by hand needs the option turned on, since a bare CMake run cannot
install OpenGL headers for you the way the installer can:

```sh
cmake -B build -DCRUCIBLE_BUILD_GUI=ON
```

The same engine with a different face — same roster, same cook loop, same config
file, same folder-trust store, no protocol in between. Dear ImGui over GLFW, in
one self-contained binary that links the core library directly.

```
┌──────────────┬────────────────────────────────────────────┐
│  ⢱⣆ CRUCIBLE │  you                                       │
│     idle     │  why does the JIT swap cost so little?     │
│              │                                            │
│  PROJECT     │  Programming · 1.00 · router model         │
│  crucible    │                                            │
│  ~/code/cru… │  ## The short answer                       │
│ Change folder│  Weights are mapped, not copied — so:      │
│              │   • the page cache holds them already      │
│  EXPERTS     │   • only the KV cache is really allocated  │
│  ◇ Mathema…  │                                            │
│  ◆ Programm… │  ┌────────────────────────────────────┐    │
│              │  │ llama_model_load_from_file(path);  │    │
│  VIEW        │  └────────────────────────────────────┘    │
│  Chat  Cook  ├────────────────────────────────────────────┤
│  History     │  ask anything                     [ Send ] │
│  Settings    │  or give it a goal to cook on     [ Cook ] │
│              │                                            │
└──────────────┴────────────────────────────────────────────┘
```

Both panels are draggable. The sidebar closes when you drag its edge to the left
of the window and comes back when you pull that edge out again; the box you type
in has a handle above it and grows to whatever height you drag it to. The sidebar
carries the folder being worked in and the expert list with live status.

**It says where it is working, and the terminal program does not have to.**
`crucible` is told where it is by being run there — you `cd`, then you type it. A
window has no `cd`, so it shows the folder instead, with one button that opens a
browser to change it. Choosing a folder goes through the same trust prompt, so a
directory trusted in one face is trusted in the other.

**Replies are rendered, not printed.** Headings, bold, lists, tables and fenced
code all draw as themselves, using the same parser the terminal uses — so both
faces break a reply into the same blocks and only the drawing differs. A cook
step expands to show its diff, coloured by line.

The typeface is JetBrains Mono, compiled into the binary. A font is the one
asset the program cannot draw for itself, and searching for it at runtime would
mean an install layout and a search path for a typeface — the same machinery the
flame mark avoids by being vector shapes.

It is opt-in because it is the only part of Crucible that needs anything from
the system beyond a compiler: OpenGL, and on Linux a few X11 development
headers. An install that cannot find them builds the terminal program and says
so.

---

## Install

One command, on any of the three.

**Linux and macOS**

```sh
curl -fsSL https://raw.githubusercontent.com/mattsaund/Crucible/main/install.sh | bash
```

**Windows** (PowerShell)

```powershell
irm https://raw.githubusercontent.com/mattsaund/Crucible/main/install.ps1 | iex
```

Either one installs **both** faces — `crucible` for the terminal and
`crucible-gui` for the desktop — along with the build tools they need, and
re-running it upgrades in place. Pass `--no-gui` (`-NoGui` on Windows) for the
terminal program alone. On Linux the desktop app needs OpenGL and a few X11
development headers, which the installer adds; where those are unavailable it
says so and builds the terminal program rather than failing.

It installs the program and the directories the program keeps its files in.
Nothing else — no compute runtime, and no models. Crucible builds a runtime on
demand from its settings screen, on the machine that will run it: a backend has
to match the hardware it is compiled for, and that is a question the program can
answer at the moment it matters and an installer can only guess at.

While it runs there is one line on screen, and it is a progress bar.

> **On platform support.** Linux is what Crucible is developed and tested on.
> macOS is regularly built and run. Windows is written and reviewed but has not
> been compiled on a Windows machine — the platform-specific parts (process
> spawning, the config directories, the resource meter) are new and unproven
> there. If you are on Windows, expect to be the first person to find the
> problems, and please report them.

## Uninstalling

One command as well, and it is the installer's.

```sh
curl -fsSL https://raw.githubusercontent.com/mattsaund/Crucible/main/install.sh | bash -s -- --uninstall
```

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/mattsaund/Crucible/main/install.ps1))) -Uninstall
```

`crucible --uninstall` does the same thing from an install that still works;
the one-liner is what to reach for when it does not.

It asks once, listing everything it is about to remove, and then removes all of
it: both programs, the libraries under them, the application-menu entry, your
configuration, the folder-trust list, the models directory, the runtimes you
built, the project history and the caches. Pass `-y` to skip the question.

**The models are yours and go with the rest.** They are the one thing in that
list Crucible did not put on disk, and a reinstall does not bring them back — so
if you want to keep a few gigabytes of GGUFs, move them out of
`~/.local/share/crucible/models` before you run it.

### Installer options

```sh
curl -fsSL .../install.sh | bash -s -- --prefix ~/.local
```

| option | |
|---|---|
| `--prefix DIR` | install location (default `/usr/local`, or `~/.local` without sudo) |
| `--no-gui` | build only the terminal program; the desktop app is built by default |
| `--jobs N` | parallel build jobs |
| `--check` | report what would happen, change nothing, never ask for sudo |
| `--no-deps` | do not install system packages |
| `-y`, `--yes` | never prompt |
| `--uninstall` | remove Crucible and everything it installed |

---

## Runtimes

Crucible supports `CUDA`, `Vulkan`, `Metal` and `CPU` runtimes.

They are stored in `~/.local/share/crucible/runtimes`, and they are entirely the
program's business — the installer does not touch them. Pick one from the
settings screen and it is compiled here, against the same llama.cpp the binary
was built from. If the SDK it needs is not installed, it says so before it
starts and gives you the exact command for your package manager, rather than
failing five minutes into a build.

## Multiple GPUs

| mode | what it does |
|---|---|
| `auto` | let llama.cpp decide (the default) |
| `even` | proportional to each card's memory, so they finish together |
| `priority` | fill the cards in the order you list, spilling into the next only when one is full |
| `single` | everything on **Main GPU** |

### Keeping the work on the GPU

Two settings under **HARDWARE** decide whether the processor and system memory
get involved at all.

| setting | default | what it does |
|---|---|---|
| **GPU-only compute** | on | every layer on the GPU, whatever **GPU layers** says |
| **Dedicated VRAM only** | off | refuse a model that will not fit in video memory |

---

## Build from source

If you would rather do it yourself: a C++20 compiler, CMake ≥ 3.24, and git.
Everything else — llama.cpp, FTXUI, nlohmann/json, and for the desktop app Dear
ImGui and GLFW — is fetched and pinned automatically.

```sh
git clone https://github.com/mattsaund/Crucible.git
cd crucible
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/bin/crucible
```

This builds the binary and no compute backend at all. Every runtime, CPU
included, is added afterwards from the settings screen — see
[Runtimes](#runtimes).

```sh
sudo cmake --install build --component crucible
# or, for a user prefix:
cmake --install build --component crucible --prefix ~/.local
```

The install is `bin/crucible` plus `lib/crucible/` holding llama.cpp's three shared
libraries. The binary's RPATH is relative, so it still works from anywhere on
`PATH`.

**Pass `--component crucible`.** llama.cpp and ggml carry their own install
rules, written for people installing llama.cpp as a library: a plain
`cmake --install` would also drop `libllama.so`, `libggml*.so`,
`ggml-config.cmake` and `ggml.pc` loose into `<prefix>/lib`. Crucible does not
use those copies — and on a system-wide install one of them could shadow
another llama.cpp. The component installs what Crucible actually needs, all of
it under `lib/crucible/`, which is also what makes `crucible --uninstall` able to
remove everything it put down.

## Build options

| option | default | what it does |
|---|---|---|
| `CRUCIBLE_BACKEND_DL` | `ON` | loadable GPU runtimes (see the note below) |
| `CRUCIBLE_NATIVE` | `ON` | tune for this machine; only consulted by monolithic builds, since a loadable backend cannot be built for one CPU |
| `CRUCIBLE_BUILD_TESTS` | `ON` | build the unit tests |
| `CRUCIBLE_BUILD_TOOLS` | `ON` | build `crucible-routebench` |
| `CRUCIBLE_BUILD_GUI` | `OFF` | build the desktop app; needs OpenGL and, on Linux, the X11 development headers. The installers turn this on by default — they can add those headers first and fall back to the terminal program if they cannot; a bare `cmake` run can do neither, so it stays off here |
| `CRUCIBLE_WARNINGS` | `ON` | strict warnings on Crucible's own sources |
| `CRUCIBLE_CUDA` | `OFF` | monolithic builds only: compile CUDA in |
| `CRUCIBLE_VULKAN` | `OFF` | monolithic builds only: compile Vulkan in |

---

## Setup

As of right now, Crucible is BYO models. There are plans in the future to train specifically trained experts to open source.

Nothing here ships with a model, and nothing downloads one behind your back.

You can either store the models in the default model directory or point Crucible to your own model directory

```
╭ Models directory ──────────────────────────────────────────────╮
│ /mnt/scratch                                                   │
├────────────────────────────────────────────────────────────────┤
│ > [ use this directory ]                         no models here│
├────────────────────────────────────────────────────────────────┤
│   ..                                                           │
│   experts/                                              9 model│
│   archive/                                             2 models│
├────────────────────────────────────────────────────────────────┤
│ ↑↓  enter open  ← up  ~ home  e type  esc    . show 32 hidden  │
╰────────────────────────────────────────────────────────────────╯
```

## Config

Type **`/settings`**. Everything in the config file is
editable there: choose the models directory with a browser, pick a model for
each expert seat and for the delegator from whatever is in it, and tune sampling.

```
╭ Settings ──────────────────────────────────────────────────────╮
│  ~/.local/share/crucible/models                  3 models found  │
├────────────────────────────────────────────────────────────────┤
│  MODELS                                                        │
│   Models directory    ~/.local/share/crucible/models             │
│                                                                │
│  DELEGATOR                                                     │
│   Router model        LFM2-1.2B-Q8_0.gguf                      │
│                                                                │
│  EXPERTS                                                       │
│ > Mathematics         math-expert-q4_k_m.gguf                  │
│   Programming         (none)                                   │
│   Physics             physics-expert-q4_k_m.gguf               │
├────────────────────────────────────────────────────────────────┤
│ ↑↓ move   enter edit   r rescan   ctrl-s save & apply   esc    │
╰────────────────────────────────────────────────────────────────╯
```
**Editing the config file**

`~/.config/crucible/config.json`:

```jsonc
{
  "models_dir": "~/.local/share/crucible/models",
  "router":   { "model": "LFM2-1.2B-Q8_0.gguf" },
  "defaults": { "n_ctx": 8192, "n_gpu_layers": -1, "temperature": 0.7 },

  // A list, because the order is the order the seats are drawn in. Crucible
  // ships no experts, so every entry here is one you made -- `/newexpert`
  // writes them; by hand, an id and a blurb are the only required fields.
  "experts": [
    { "id": "mathematics",
      "name": "Mathematics",
      "blurb": "algebra, calculus, proofs, geometry, statistics, probability",
      "model": "math-expert-q4_k_m.gguf" },

    { "id": "physics",
      "name": "Physics",
      "blurb": "mechanics, thermodynamics, relativity, quantum, electromagnetism",
      "model": "physics-expert-q4_k_m.gguf" },

    // Everything except id and blurb is worked out for you if you leave it out.
    { "id": "rust-async",
      "name": "Rust Async",
      "tag": "RA",
      "blurb": "tokio, futures, pinning, async traits, executor tuning",
      "examples": ["why does my future never wake",
                   "how do I pin a self-referential struct"],
      "keywords": ["tokio", "futures", "pinning", "async"],
      "model": "" },

    { "id": "general",
      "name": "General",
      "blurb": "anything that does not obviously belong to one of the others",
      "model": "generalist-q4_k_m.gguf" }
  ],

  // Which seat catches what the delegator could not place, or what was routed
  // to a seat with no model. Any ordinary expert; empty means there is none.
  "routing": { "default_expert": "general" }
}
```

A fresh install has an empty expert list, and an `"experts"` key that is present
but empty is taken at its word. Crucible ships no experts and never adds one
behind your back: every seat is one you made, with `/newexpert`, the GUI's
Experts page, or by writing an entry here.

```jsonc
"routing": {
  "min_confidence": 0.60,       // below this, treat the answer as undecided
  "default_expert": "general"   // catches what does not fit; "" for none
},
"tools": {
  "web_search": false,          // the only thing Crucible sends off the machine
  "workshop": false,            // let experts read and write in the project
  "workshop_run": true,         // ...and run commands there
  "workshop_timeout": 120       // seconds before a stuck command is killed
}
```

## The delegator model

The delegator never answers you; it only names an expert. 

It scores every expert on the roster as a continuation of your prompt and takes
the best. Scoring rather than generating is what makes naming an expert that
does not exist impossible rather than merely unlikely, and it gives a confidence
worth thresholding on — you can set that threshold in settings. The more
detailed the prompt, the more accurate the delegator is.

Adding an expert changes what it is choosing between, which is why a new seat
gets worked examples written for it before it is first used.

---

## Commands

| command | |
|---|---|
| `/<expert> <prompt>` | skip routing, send straight to one expert |
| `/cook [30m] <goal>` | work on this project until the time is up, or until `/stop` |
| `/stop` | wrap up the cook: finishing touches, then done |
| `/cooks` | what past cooks changed, and how long they took |
| `/newexpert` | add an expert: a name, and what it is trained in |
| `/ejectexpert <name>` | remove one from the list |
| `/experts` | which seats are filled, and with what |
| `/resume` | reopen an earlier conversation about this project |
| `/new` | start a fresh conversation, keeping the current one on disk |
| `/usage` | tokens spent this session and on this project |
| `/settings` | assign models, tune sampling, choose hardware |
| `/runtimes` | install or remove compute backends |
| `/models` | list the .gguf files in the models directory |
| `/devices` | compute devices, with the indices the GPU split uses |
| `/effort low\|medium\|high` | how hard a thinking model works |
| `/thinking` | show or hide a thinking model's working |
| `/search <query>` | look something up, if web search is on |
| `/release` | unload the resident expert, freeing its memory |
| `/clear` | clear the transcript and the experts' history |
| `/paths` | where the config, models, runtimes, history and log live |
| `/help`, `/quit` | |

Type `/` and the list folds up out of the prompt, narrowing as you type, with
the rest of the best match in grey after the cursor. `Tab` takes it:

```
 › /resume   reopen an earlier conversation about this project
   /release  unload the resident expert and free its memory
   tab completes   ↑↓ choose   esc dismiss
 › /resume
     ▲── typed "/re"; "sume" is the suggestion
```

| key | |
|---|---|
| `Tab` | accept the suggested command |
| `↑` `↓` · wheel | scroll the transcript a line at a time |
| `Ctrl-C` | cancel the current answer; again when idle to quit |
| `Ctrl-T` | show/hide the expert panel |
| `PgUp` / `PgDn` | scroll the transcript a page at a time |

### Resuming a conversation

Crucible keeps history per project — the directory you started it in. `/resume`
lists what it has for *this* project and nothing else:

```
 resume · crucible
 ▸ 2 hours ago    why does the JIT swap cost so little?     6 turns   4.1k tok
   yesterday      explain the grammar-constrained sampler   3 turns   2.2k tok
   12 Aug         first pass at the router prompt          14 turns  18.3k tok

 ↑↓ choose   enter resume   d delete   esc cancel
```

`enter` restores the transcript **and** hands the exchanges back to the expert,
so the next question continues the conversation rather than starting cold.
Further turns append to the same session. `d` twice deletes one.

A session is written after each completed turn, so a crash costs at most the
turn in flight; a reply still streaming is never saved, because it is not
something to resume into. `/new` starts a fresh one without discarding the old.

History lives in `~/.local/share/crucible/projects/<name>-<hash>/`. The hash is
what keeps two different checkouts called `src` apart.

---

## File Structure

src/
├── main.cpp        parse, trust, hand off -- 40 lines
├── app/            things that happen instead of the TUI
│   ├── cli.cpp         argument parsing, banner, usage
│   ├── trust_gate.cpp  the folder-trust prompt
│   └── uninstall.cpp   crucible --uninstall
├── config/         what lives on disk
│   ├── config.cpp      the Config type's own behaviour
│   ├── config_io.cpp   reading and writing config.json
│   ├── gpu_policy.cpp  turning a split mode into tensor_split
│   ├── paths.cpp       XDG locations
│   └── trust.cpp       the folder-trust store
├── runtime/        the loadable compute backends
│   ├── backend.cpp     the backend table; everything derives from it
│   ├── registry.cpp    what is installed, and handing it to ggml
│   ├── builder.cpp     compiling one on demand, off the UI thread
│   └── devices.cpp     device enumeration and the GPU split policy
├── session/        what a conversation costs, and remembering it
│   ├── usage.cpp       token counting and its readout
│   └── store.cpp       per-project session history
├── routing/        deciding who answers
│   ├── expert.cpp      the roster; every delegator input is generated from it
│   ├── router.cpp      KeywordRouter and ModelRouter
│   ├── benchmark.cpp   the prompts the delegator is measured against
│   └── completion.cpp  the slash-command list, and matching a prefix to it
├── cook/           working on a project over an hour
│   └── journal.cpp     what a cook did, live and afterwards
├── llm/            everything that touches llama.cpp
│   ├── model_host.cpp    owns the backend; one expert resident at a time
│   ├── loaded_model.cpp  the generation loop
│   ├── sampling.cpp      building a sampler chain
│   ├── model_catalog.cpp reading the models directory
│   ├── model_shape.cpp   what a GGUF says about itself, before it is loaded
│   └── response_filter.cpp  sorting a model's working from its answer
├── tools/          what the experts can reach beyond the machine
│   ├── web_search.cpp  looking something up, off by default
│   └── workshop.cpp    reading, writing and running things inside one root
│
├── util/           the parts with no opinions
│   ├── diff.cpp        what a rewrite actually changed
│   ├── markdown.cpp    reading the markdown a model wrote
│   ├── platform.cpp    the three things that differ per operating system
│   ├── resources.cpp   what the GPUs and the processor are doing
│   ├── subprocess.cpp  fork/exec with a merged output pipe
│   ├── format.cpp      bytes, durations, paths
│   └── text.cpp        UTF-8 that arrives a fragment at a time
│
├── engine/         the delegation loop            [worker thread]
│   ├── engine.cpp        route -> JIT swap -> generate
│   ├── engine_cook.cpp   the cook loop: act, read the result, act again
│   ├── route_policy.cpp  what to do with the delegator's answer
│   └── state.cpp         the only memory the two threads share
├── ui/                                            [UI thread]
│   ├── app.cpp         shell, key handling, animation clock
│   ├── commands.cpp    slash commands
│   ├── transcript.cpp  drawing the conversation, markdown and all
│   ├── session_picker.cpp  the /resume list
│   ├── widgets/        flame sprite, expert panel, new-expert form
│   └── settings/       view, runtimes panel, GPU priority panel,
│                       model manager, directory browser, model picker,
│                       line editor
└── gui/            the desktop app                 [its own binary]
    ├── main.cpp        parse, trust, hand off
    ├── app.cpp         the window: sidebar, panes, composer, dialogs
    ├── markdown_view.cpp  drawing the markdown a model wrote
    └── theme.cpp       the palette, the flame, the fonts

packaging/          the mark, and the application-menu entry
├── flame.txt       the flame, in braille -- the one copy the banner at the
│                   top of this file, `crucible --help` and both installers
│                   are pasted from
├── crucible.svg    the same flame as a vector, for the launcher icon,
│                   generated from theme.cpp's own control points
└── crucible.desktop.in   the XDG entry, with the installed path filled in

The terminal program draws it a third way: `src/ui/widgets/flame_sprite.cpp`
carries the same drawing resampled to nine columns by seven rows, at four
heights, because that one has to animate and has to fit beside a roster of
seats. It is braille too, with the same silhouette in hashes behind it for the
terminal whose font has no braille in it.

## Roadmap

- [x] JIT model host, router, expert-panel TUI, streaming, cancellation
- [x] Models directory, and every setting editable in the app
- [x] Routing benchmark, and a delegator prompt tuned against it
- [x] A nominated default expert for prompts the delegator cannot place
- [x] Loadable runtimes — install and remove CUDA / Vulkan / CPU from settings
- [x] Token counts per turn, session and project, with a live tok/s readout
- [x] Multi-GPU splitting: even by memory, or a priority order you set
- [x] `/resume` — per-project conversation history
- [x] **A roster you own** — `/newexpert` and `/ejectexpert`, with the delegator
      writing its own worked examples for a new seat
- [x] **Agentic tools** — file read/write, shell, web search, sandboxed to one
      root and off until switched on
- [x] **Cooking** — a goal and a budget instead of a question, with a journal of
      what changed
- [x] **A desktop app** — the same engine, a different face, with a project
      picker, rendered markdown and a resizable sidebar
- [x] **Handover mid-cook** — the next piece of work goes back through the
      delegator, so a cook can pass from a code expert to a writing one
- [ ] **Sandbox `RUN`** — bubblewrap on Linux, seatbelt on macOS, so a command
      is confined the way a file path already is
- [ ] **Compile Crucible on Windows** — the platform code is written; nobody has
      built it there yet
- [ ] Prefix caching so an unchanged conversation is not re-ingested every turn
- [x] **Diffs in the cook journal** — a write records what changed, not only
      that something did, and both faces expand a step to show it
- [ ] Predictive preloading of the likely next expert
- [ ] Fine-tuned 1B Crucible router to replace the off-the-shelf one
- [ ] Curated subject experts, offered as a download you opt into — never bundled

---

**AI Policy**

I am open to AI and agentic coding, but the code written needs to follow specific guidelines:
1. MUST be human readable, acceptable variable/function names.
2. easily tracable, following a good program flow
3. Contributer MUST look at/document code and code changes. you need to understand the code that is being written.

---

## License

MIT — see [LICENSE](LICENSE). Every dependency Crucible links is MIT too, and
[THIRD_PARTY.md](THIRD_PARTY.md) lists them with their pinned versions. Nothing
is vendored into this repository; the build fetches each at a fixed tag.

Model weights are covered by none of it. Crucible ships no models and downloads
none — whatever GGUFs you put in your models directory carry their own licenses,
which are between you and whoever trained them.

---