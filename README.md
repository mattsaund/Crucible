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

**Crucible** is a local LLM engine that uses delegation and circular computing to improve the performace of local AI models. 

**Delegation**
Crucible uses an LLM to analyze the given prompt, and delegate it to a specifically trained expert that the user provides. Essentially, having 10 30 billion parameter models specifically trained on seperate subjects would outperform 1 30 billion parameter MoE model. Crucible uses JIT loading so models are not loaded at the same time. 

**Circular Computing**
the user can set a specific end goal and have the entire system, delegator and experts, work on the same prompt or project over and over again. Each pass, the project improves. You can have this run for as long as you want.

___
## Install

One command.

**Linux and macOS**

```sh
curl -fsSL https://raw.githubusercontent.com/mattsaund/Crucible/main/install.sh | bash
```

**Windows** (PowerShell)

```powershell
irm https://raw.githubusercontent.com/mattsaund/Crucible/main/install.ps1 | iex
```

This single line installer installs both the CLI and the GUI interface. it does not install any runtimes or models.

___
## Uninstalling

One command as well.

```
crucible --uninstall
```

If the binaries are gone you can uninstall and clean up with this command

**Linux and macOS**

```sh
curl -fsSL https://raw.githubusercontent.com/mattsaund/Crucible/main/install.sh | bash -s -- --uninstall
```

**Windows**

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/mattsaund/Crucible/main/install.ps1))) -Uninstall
```

___
## Installer options

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
## The experts

Crucible is 100% BYO model. You need to specifically train experts in subjects and load them into Crucible using the `/newexpert` command on the CLI tool, or in the settings of the GUI application.

Setting up an expert is easy. You name it, describe what it is trained in, and the delegator model will take care of the routing, keywords, and backend work to link the expert into the system.

You get rid of an expert by ejecting it. Either `/ejectexpert [expert name]` in the CLI tool or in the settings of the GUI application.

---
## Cooking

You can have Crucible **Cook** on a specific task for an arbitrary amount of time. This gives the local models the ability to improve opon itself over and over again.

```
/cook fix the failing tests and tidy up the parser
/cook 30m make the CLI take a --json flag
/stop
```

A cook is a goal. It runs until the work is done or until you `/stop` it.

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

The delegator stays active and can handoff work to another expert mid cook

```
     done     fixed the sign error and confirmed the test passes
     handoff  document the parser's public functions
     note     Programming handed over to Language
     write    updated README.md  +18
```

___
## Acting on the project

Trusting a folder is what lets an expert act on it rather than describe it.
Crucible asks once, on first use in a directory, and yes means yes — there is no
second switch behind it. The same verbs are available in Chat and in Cook,
because "fix the typo in README" is a sentence rather than a goal worth starting
a cook for, and the answer to it is the edit:

| | |
|---|---|
| `LIST:` | what is in a directory |
| `READ:` | a file, with line numbers |
| `WRITE:` | replace a file |
| `RUN:` | run a command, starting in the project root |
| `SEARCH:` | look something up (needs web search on) |
| `NOTE:` | record what it is doing |
| `ASK:` `DONE:` `HANDOFF:` | cook only — ask you something, close a piece of work, hand the next piece to a different expert |

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

**So the trust prompt says so.** It used to be two more switches under a
settings page, on top of the folder trust — which meant you could answer the
trust question yes and still find that nothing could be edited, with no
indication of which box you had not ticked. The question is asked once now, in
the terms it is actually about: read, write and run commands in this folder,
paths outside it refused, and a command it runs is a command.

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
┌─────────────────────────────────────────────────────────────────────┐
│ ▣  ⢱⣆ CRUCIBLE      Project: ~/code/crucible   Chat  Cook  History ⚙│
├───────────────┬─────────────────────────────────────────────────────┤
│ loading 41%   │   ▌why does the JIT swap cost so little?            │
│ DELEGATOR     │                                                     │
│ ◆ qwen3-4b    │   ◆ Programming · 94% · router model · swap 1.2s    │
│ │             │                                                     │
│ EXPERTS       │   ## The short answer                               │
│ │ ◇ Mathema…  │   Weights are mapped, not copied — so:              │
│ └─◆ Programm… │    • the page cache holds them already              │
│   ◇ Writing   │    • only the KV cache is really allocated          │
│   · Science   │                                                     │
│               │   ┌ c++ · model_host.cpp ──────── 4 lines ─ ⧉ ┐     │
│               │   │ 41   auto* model = llama_model_load(...);│     │
│  [ Eject ]    │   └──────────────────────────────────────────┘     │
├───────────────┤─────────────────────────────────────────────────────┤
│ 12k in/4k out │   ask anything                            [ Send ]  │
└───────────────┴─────────────────────────────────────────────────────┘
```

**The top bar is everything true of the window.** Which project is open — name,
path, and a click to change it — the fold button for the side menu, the three
views, and Settings in the corner. Settings is a corner rather than a fourth tab
because it is where you go to change the program rather than one of the three
things the program does; the gear goes in, and the same gear comes back out to
where you were.

**The side menu is one picture of how a prompt gets answered.** The delegator on
top, the experts indented under it, a status diamond on every one of them — and
while a turn is flowing, a line drawn from the delegator's dot down and into the
seat it chose. It is the same drawing the terminal makes in its expert panel,
turned ninety degrees. Yellow means "this one has the turn", the delegator
included; white means it can answer; faint means it has no model behind it.

**What it is doing is said over the models it is about.** "loading router 41%"
sits above the delegator rather than in the top bar, because it is a sentence
about the column under it and true of one model for thirty seconds — where the
bar is for what is true of the window. The row is always there, empty when
nothing is happening, so the list below it never shifts by a line.

**Eject unloads everything.** Both models, not just the expert: with the
delegator set to stay resident it is the larger of the two on plenty of
machines, and dropping only the expert leaves a card holding a model nothing is
about to use — which is the state people press Eject to get out of, usually
because they want the memory for something that is not Crucible. Whatever is
needed comes back on the next prompt. It sits above the rule at the bottom of
the side menu, sized to its own word rather than to the panel: a button
stretched across a sidebar you can drag to four hundred pixels reads as the most
important thing on screen, and this is a control you press once in a while.

**What the conversation costs is written under the box you type in.** Tokens in,
tokens out, and how much of the expert's context the last turn filled — `1.2k in
Â· 253 out Â· 62% context used`. It moved there from the foot of the side menu,
where it sat under a list of experts it had nothing to do with: both numbers are
about the conversation, and the conversation is on this side of the window. The
percentage is the one that changes behavior, and it turns amber past three
quarters — the point at which the next turn starts dropping earlier exchanges.

Both panels are draggable. The sidebar closes when you drag its edge to the left
of the window — or with the fold button — and the box you type in has a handle
above it and grows to whatever height you drag it to.

**It says where it is working, and the terminal program does not have to.**
`crucible` is told where it is by being run there — you `cd`, then you type it. A
window has no `cd`, so the top bar shows the folder instead, and clicking it opens
a browser to change it. Choosing a folder goes through the same trust prompt, so a
directory trusted in one face is trusted in the other.

**Replies are rendered, not printed.** Headings, bold, lists, tables and fenced
code all draw as themselves, using the same parser the terminal uses — so both
faces break a reply into the same blocks and only the drawing differs.

**Code is colored, numbered, and read as a diff when it is one.** A fenced block
carries a header saying what language it is and which file it belongs to, numbers
every line down the left, and colors the code with a small lexer that covers
twenty-odd languages (`src/gui/syntax.cpp`). A unified diff is recognized without
being told: the marker in column one becomes a gutter of old and new line
numbers, added rows are washed green and removed rows red — and the code on each
row keeps its syntax colors either way, because an added line you cannot read is
not much of an improvement on a removed one. A block longer than about forty
lines folds itself, with one click to open it out.

**Chat and Cook differ in how long they run, not in what they may touch.** Chat
is one question and one answer: the delegator reads what you type, hands it to
the expert it fits, and that expert answers — reading, writing and running things
in the project if the answer calls for it. Cook is one goal worked in passes —
read, change, run, judge, go round again — with the goal pinned at the top, the
pass count beside it, and every step accumulating underneath, each folding open
to the diff or the command output it produced.

**Auto mode decides whether you see an edit before it lands.** The toggle sits
beside the box you type in, because whether you are watching is a decision that
changes between one prompt and the next rather than a setting you go and find.
Off -- the default -- every file an expert wants to write stops and shows you
the file as it is beside the file as it would be, and nothing is written until
you pick one. It is two whole files rather than a diff on purpose: a diff is the
right way to review a change you have already decided to take and the wrong way
to decide, because it shows what moved and hides what the file becomes. On, the
edit lands and you read about it afterwards.

A file that does not exist yet is a different question, so it gets a different
answer: one panel with the path and the contents, and **Allow** or **Deny**.
There is no "before" to compare it against, and a column headed "Now" saying
"this file does not exist" asks the reader to compare something with nothing.

Where there *is* a before, the lines that differ are washed -- red on the left
for what goes, green on the right for what arrives -- with the count in the
header. Whole files answer "which one do I want" and are useless at "what is
different"; on a four-hundred-line file with one function changed that is the
only question anyone has, so both are on the screen at once.

Cook always applies. A cook is an hour of work you started and walked away from,
and stopping it on the first write to ask a question nobody is there to answer
would mean it never gets past the first write -- its record is the journal, and
every step in it expands to the diff that step made.

**What a turn did stays on the screen.** The diff a write made and the output a
command printed are kept for the life of the turn and scroll back with it. They
used to be thrown away the moment the next round started -- the tool call was
wiped off the transcript as "a request, not an answer" and everything it
produced went with it, so scrolling back through an hour of work found the
summaries and none of the code. The sentence the expert wrote *around* the call
stays too; only the protocol line goes.

**A model's working folds away.** Reasoning sits behind a disclosure triangle on
every turn that has any, so a long think does not push the answer a screen
down -- and a reasoning model that spent its whole budget thinking can still be
asked what about. The setting in Settings decides whether a turn opens expanded,
which makes it a default rather than a verdict.

**A code block is colored even when nothing said what it was.** Models open a
fence with a bare ``` about as often as they name the language, so an unlabeled
block is sniffed from its own contents -- a shebang, an `#include`, `fn main(`,
`<?php`. A weak guess is no guess: a paragraph of English with one `const` in it
stays plain text, because a confident wrong answer is worse than none.

The terminal draws the same code blocks and asks the same question. Both faces
share `util/syntax.cpp` and `util/code_lines.cpp`, so a keyword is the same
color and a hunk starts on the same line in either -- only the painting differs.
There the two files stack instead of sitting side by side, because eighty
columns cannot hold both and stay readable, and the choice is a list you steer
with the arrow keys rather than two panels you click:

```
  Create hello.py

  python                                                    1 line
  1 print("hello")

  ❯ Create the file
    Do not create it
    arrows to choose, enter to take it · y / n · esc leaves it alone
```

`/auto` turns the asking off and on, and `/showthinking` and `/hidethinking` do
for a model's working what the triangle does in the window. Two verbs rather
than one toggle: a toggle has to be read before it can be used, and by the time
you are asking which way it is set the answer has scrolled off the top.

**Every turn can be stopped, asked again, or thrown away.** Hovering an exchange
puts the controls for it at its top right, and nowhere else: a transcript with
three buttons beside every entry is a control panel with a conversation in it.
Stop appears only on the turn that is actually running -- and it stops a model
that is still coming off the disk, not just a reply mid-flight, which is the
difference between a program you can interrupt and one that has frozen for the
minute a thirty-gigabyte expert takes to load. Asking again drops that turn and
the ones after it before resending, so the expert sees the same context it saw
the first time; deleting takes the question and its answer out of the expert's
memory as well as off the screen.

**The delegator is freed after every decision by default.** One model is
resident at a time: the delegator routes, is released, the expert loads,
answers, and is released in its turn. The peak is the larger of the two rather
than their sum, which is what leaves an expert the whole card. Settings has a
checkbox to keep it resident instead, which is the right trade only when the
delegator is small next to the card -- otherwise every expert that follows pays
its whole footprint for the rest of the session.

With nothing in either view yet, they say one of three things and nothing else:
**No runtime** (there is no backend installed to run a model on), **No model
selected** (no expert has a GGUF behind it), or **Ask anything**. The first two
are the way to the page that fixes them.

The typeface is JetBrains Mono, compiled into the binary. A font is the one
asset the program cannot draw for itself, and searching for it at runtime would
mean an install layout and a search path for a typeface — the same machinery the
flame mark avoids by being vector shapes.

It is opt-in because it is the only part of Crucible that needs anything from
the system beyond a compiler: OpenGL, and on Linux a few X11 development
headers. An install that cannot find them builds the terminal program and says
so.

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

Pick `priority` and the card list becomes an order you can rearrange with
arrows: the card at the top is filled first, and the next one is only reached
once it is full. In the other three modes the arrows are gone and the list is
just the cards and what is free on each — an order nothing reads is a control
that looks broken, because rearranging it changes nothing and nothing on screen
says why. An index left over from a card that is no longer in the machine is
dropped the moment Crucible can see the real hardware, so a stale entry cannot
quietly shift everything below it.

What priority decides is *how much* of the model each card gets, not which
layers land on it. `tensor_split` is a proportion per device and llama.cpp walks
devices in its own index order, so the card you put first gets the largest share
and takes whichever layers fall to it.

Crucible keeps 128 MiB per card in reserve when it checks whether a model will
fit. It is deliberately small: a check that refuses a model which would have
worked is worse than the abort it exists to prevent.

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
  "workshop_timeout": 120,      // seconds before a stuck command is killed
  "overflow": "rolling"         // "rolling", "middle" or "stop" -- see below
}
```

### When the conversation outgrows the context

A context window is finite and a conversation is not. Three quarters of the
window is the conversation's; the last quarter is left for the answer, because a
prompt that fills the window leaves nothing to reply with. What happens when the
conversation wants more than its three quarters is yours to pick, under
**Settings → Tools → Context** in the window and `Context overflow` in the
terminal.

| `overflow` | in settings | what it does |
|---|---|---|
| `rolling` | Rolling window | drop the oldest exchanges until it fits (the default) |
| `middle` | Truncate middle | keep the beginning and the end, drop what is between |
| `stop` | Stop at limit | drop nothing; refuse the turn and say so |

`rolling` is right for a conversation where what was said an hour ago matters
less than what was said a minute ago. `middle` is right for one that opened with
something that has to survive — a specification, a file, a set of rules — and has
since wandered. `stop` is for when a silently shortened conversation is worse
than no answer: the model cannot tell you what it stopped being able to see, so
the program says it instead and leaves the turn unrun.

Two things are never dropped, whatever the setting: the system prompt and the
question you just asked. Exchanges go whole, question and answer together — half
an exchange is an answer with no question, which the model reads as something it
got wrong. When a turn does drop something it says so in the transcript, on its
own line under the route: *dropped 2 earlier exchanges to stay inside the
context*.

The counting is done by the expert's own tokenizer through its own chat
template, not by an estimate — the figure being compared against the window is
the figure the model will actually see.

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
the rest of the best match in gray after the cursor. `Tab` takes it:

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
│   ├── config.cpp      the Config type's own behavior
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
│   ├── overflow.cpp      making a conversation fit in the context window
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
    ├── app.cpp         the window: the frame, the reading column, the actions
    ├── markdown_view.cpp  drawing the markdown a model wrote, code blocks and
    │                      diffs included
    ├── syntax.cpp      the lexer the code blocks are colored with
    ├── widgets.cpp     the vocabulary the panels are written in
    ├── theme.cpp       the palette, the flame, the fonts, the title-bar marks
    └── panels/         topbar, sidebar, chat, cook, settings, dialogs

packaging/          the mark, and the application-menu entry
├── flame.txt       the flame, in braille -- the one copy the banner at the
│                   top of this file, `crucible --help` and both installers
│                   are pasted from
├── crucible.svg    the same flame as a vector, for the launcher icon,
│                   generated from theme.cpp's own control points
└── crucible.desktop.in   the XDG entry, with the installed path filled in

___
## AI Policy

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
