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

**Crucible** runs a roster of small, subject-specific models on your own machine,
routes each prompt to the right one, and lets you build the missing ones
yourself. Ten specialists beat one generalist of the same size, and only one is
in memory at a time.

And it builds software. Give it a directive -- what you want, in a sentence or
a page -- and an architect plans it, the plan is cut into tasks, each task
goes to the expert that fits it (a frontier model for the heavy lifting, a
local one for the rest), the tasks are built, checked, committed and written
up, and the result is previewed, tuned, version-controlled and shipped from
the same window. You bring the idea, the design and the vision.

Website: https://msaunders.dev/crucible/

![Crucible routing a question to one of its experts](docs/images/chat.png)

___
## Install

**Download and double-click** — from the
[releases page](https://github.com/mattsaund/Crucible/releases):

| | | |
|---|---|---|
| macOS | `Crucible-macOS.dmg` | drag Crucible into Applications |
| Windows | `Crucible-Setup.exe` | run it; no administrator prompt |
| Linux | `Crucible-x86_64.AppImage` | `chmod +x` it and run it |

Nothing is signed with a paid certificate, so Windows shows a SmartScreen
warning (More info → Run anyway) and macOS refuses the download the first time:
open Crucible once, then **System Settings → Privacy & Security → Open Anyway**.
One-time answers on both.

**On its first start it fetches what the download does not carry**, in the
order it is needed, with the progress above the empty chat and nothing to
click:

- its own Python (3.12, about 25–70 MB depending on the platform), which
  routing and cooks run on — no Python on your machine is needed or touched;
- the compute runtimes this machine can use: the CPU always, CUDA with
  NVIDIA's libraries where there is an NVIDIA driver, Metal on a Mac, Vulkan
  elsewhere (a Mac can add Vulkan from Settings, Runtimes, but is not asked
  to compile it on its first start);
- the training environment for Create, built from that Python — a few
  gigabytes with CUDA, and on a Mac also what runs MLX models.

A chat works as soon as Python and a runtime are in. Anything that fails says
why and has **Try again**.

**Or build from source, in one command:**

```sh
curl -fsSL https://raw.githubusercontent.com/mattsaund/Crucible/main/install.sh | bash
```

```powershell
irm https://raw.githubusercontent.com/mattsaund/Crucible/main/install.ps1 | iex
```

- Builds the program and installs an application entry you can pin to a dock or
  taskbar. Typing `crucible` in a terminal starts the same thing.
- It opens on no project: a chat works in a scratch folder of its own under
  `~/Crucible/Scratchpad`, and the top bar shows the folder's path, until you
  open a project from the panel on the right.
- Options: `--prefix DIR`, `--jobs N`, `--check`, `--no-deps`, `--no-trainer`,
  `-y`, `--uninstall`.
- Run either installer from inside a clone and it builds that clone.
- Crucible's own Python and the compute runtimes this machine can use are put
  in place too, so the first start has nothing left to fetch but what you
  skipped. No models are installed.
- **The fine-tuner is set up**: a private Python environment in Crucible's own
  data folder, a few gigabytes, built from Crucible's own Python rather than
  the machine's. It cannot break a system package and `crucible --uninstall`
  takes it with everything else. `--no-trainer` skips it (the window fetches
  it on first start instead); `crucible --install-trainer` adds or repairs it.

**Updating:** the same command that installed it. Crucible checks GitHub once a
day for a newer release; when there is one it says so in the transcript and
**Settings → About** names the version and the line to run. Downloads are
replaced by the newer download from the releases page. Your config, models and
history stay where they are — an update replaces the program and nothing else.
The check is one request for a public version number, it says nothing about the
machine, and the checkbox beside it turns it off.

Versions are `MAJOR.MINOR.PATCH`, tagged `v0.9.0` on GitHub, and the release a
tag builds carries the installers for all three platforms. What changed in each
is in [CHANGELOG.md](CHANGELOG.md).

**Uninstall:** `crucible --uninstall` removes the program, the config, the trust
list, the history, the fine-tuner's Python environment and — after naming them
and asking — the models. If the binary
is gone or too broken to run, the installers do it instead:

```sh
curl -fsSL https://raw.githubusercontent.com/mattsaund/Crucible/main/install.sh | bash -s -- --uninstall
```

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/mattsaund/Crucible/main/install.ps1))) -Uninstall
```

___
## What it does

### Delegate

- A small router model scores every expert on your roster and sends the prompt
  to the best fit, with a confidence you can set a threshold on. The deciding
  is done in Python, in a process of its own on Crucible's Python, and the
  model is asked through the core — so how routing works can be changed and
  measured (`crucible-routebench`) without rebuilding anything.
- One model is resident at a time: the router is released, the expert loads,
  answers, and is released in its turn. Peak memory is the larger of the two,
  not the sum.
- Naming an expert that does not exist is impossible — the router scores the
  roster rather than generating a name. It is shown the roster in three
  orders and the answers averaged, because a small model leans towards
  whichever expert it sees first.
- Nothing is loaded until a prompt needs it: opening Crucible costs the card
  nothing. Bring your own models — Crucible downloads none.
- Or skip it: the menu beside the **+** in the box sends every prompt to one
  expert until you change it back, and `/name` does it for one prompt. The
  menu next to it sets how hard a reasoning model thinks; a model with no
  such setting is never sent it.

![The roster: five seats, each with a description and a model](docs/images/experts.png)

### Build

- Type a directive into the **Build** tab — "a command-line todo app in
  Python, with tests" — and an **architect** (the seat Settings, Build names,
  or the one the delegator picks) writes a plan: tasks, what each needs, which
  must come first, and the commands that run, check and package the result.
  The plan is shown before the first task starts; type *go*, or what to change.
- Each task is routed the way a HANDOFF is: the delegator is shown what it
  needs and picks the seat. Tasks run one after another, each with the full
  workshop below, each ending with DONE and an account the next agent reads.
  A task that stalls or runs out of rounds is left incomplete and the build
  moves on; one whose dependency failed is skipped and says so.
- **Agents** is the view: down one side the architect and a row per task with
  the expert that has it and where it got to; on the other, the one you click
  — what it was asked, every step, the files it changed, which open in the
  code pane. A build can ask you things on the way, and **Stop and finish**
  leaves the project running.
- **The experts it needs.** When nobody on the roster fits a task and
  Settings, Build names a model for new agents, the build adds a seat for the
  expertise — "CSS Layout", "SQL Schema" — and keeps it, with its routing
  examples written by the delegator. The side menu lists those under
  **Agents**, below the experts you added yourself; an agent is routed to,
  edited and ejected like any seat. And every task a provider's
  model did is recorded, with the files it made, in the project's history
  folder, as data a local expert can be fine-tuned on in Create: the frontier
  model teaches the local one the project.
- After each finished task the build **commits** what changed, starting a
  repository when there is none, so a bad task is one revert. When the plan
  named a check, it is run at the end; if it fails, one more task makes it
  pass. Then the architect writes up what was built, how to run it, and what
  is left.
- **Tasks run one at a time.** The engine has one worker and one local model
  resident, and a build that pretended otherwise would be queueing behind
  itself. Chat and Cook wait for a build the way they wait for each other.

### Code, Source, Preview

- Three panes beside the agents, for any project whether a build is running
  or not. **Code** is the project's files: a tree, a file opened to read, or
  to edit and save with Ctrl+S, or a picture looked at.
- **Source** is version control: the branch and remote, what changed since
  the last commit with the diff of any file, a commit box, Pull and Push, a
  box for any git command, and the history. With `gh` installed, **Publish
  to GitHub** makes a repository for the project and pushes it. **Ship**
  runs the command that packages it — the plan's, or yours — and tags a
  GitHub release.
- **Preview** shows an HTML page of the project in a frame, its stylesheets,
  scripts and pictures put inside it by Crucible, or a page by address — the
  server a build left running with START. **Pick an element** and a dashboard
  beside the frame shows what it is and the things most worth changing by
  eye: padding, margin, gap, font size, font, weight, color, background,
  rounding, alignment, width — applied to the page as you change them, with
  a plus and a minus to nudge the lengths. **Apply to source** hands the set
  to an expert as a Chat prompt to make in the stylesheet.
- Crucible runs the `git` and `gh` the machine has, in the project folder,
  and ships neither.

- A build is also how a thing is made better: a directive about a project
  that exists — "make the failing tests pass", "add dark mode" — is planned
  and worked the same way. Every step is journaled and folds open to the diff
  or command output it made, and a build can stop to ask you a question or
  hand a task to a different expert. (There was a Cook tab for this, one goal
  worked in passes until stopped; it is folded into Build, and a cook from
  before still opens from History.)

### Attach

- The **+** at the bottom left of the box, or Ctrl+U, attaches files, photos
  or a whole folder through the platform's own file dialog — to a question or
  to a cook's goal. Or drag them onto the window: it blurs behind an orange
  outline and takes whatever you let go of. They sit in the box as tiles, and
  you keep typing.
- PDFs, Word (`.docx` and `.doc`), Excel, PowerPoint, OpenDocument, RTF, EPUB,
  HTML, Markdown, code, CSV and any other text are read into the prompt above
  what you typed, each under its own name. A folder sends its readable files,
  skipping dependencies, build output and version control.
- Each attachment is cut to what fits the answering expert's context, and the
  prompt says where it was cut.
- Pictures go to a provider's model as pictures, shrunk to what it takes. A
  model on this machine reads text only and is told a picture was attached
  that it cannot see.
- Nothing is converted anywhere but here.

### Create

- The tab is the experts you have made: what each one is for, what it was built
  from, and whether it is a draft, training, ready to test or finished.
- **New expert** is a wizard — a base model and a dataset, browsed from
  Huggingface inside the app or brought from your disk, then quantization,
  epochs, context and learning rate. It estimates what will fit on your card
  before the run rather than forty minutes into it.
- Starting a run closes the wizard and puts the model in the list; clicking it
  shows its specs and its progress. The finished file is written into your
  models folder, so it is chosen for a seat -- or as the delegator -- like any
  other model there.
- Testing opens a window that really runs the candidate. **Finish** keeps it
  and gives it a seat the delegator can route to; **Edit** goes back to the
  fine-tune parameters.
- Training runs on your card, in Crucible's own Python environment — LoRA, or
  QLoRA where there is CUDA. The page shows the step count, the loss curve,
  which card it picked and how long is left, and stopping takes effect within
  a step.
- Which PyTorch gets installed follows the cards that are in the machine. A
  wheel built for older architectures does not run slowly on a newer card, it
  does not run at all — while still reporting that CUDA is available — so the
  environment names any card its torch has no kernels for.
- **In progress:** a run belongs to the window that started it and stops if
  you close Crucible; there is no checkpoint to resume from. Export is GGUF at
  F16 or Q8_0 — the K-quants need `llama-quantize`, which is not built here
  yet, and a run says so rather than quietly giving you something else. A
  model trained elsewhere with unsloth, axolotl or mlx_lm can be attached to a
  recipe instead.

![The experts made here, grouped by how far along each one is](docs/images/create.png)

![Talking to a fine-tune before deciding whether to keep it](docs/images/create-test.png)

![The training environment: what each build can do, and what it costs to install](docs/images/training.png)

### Act on a project, and beyond it

- Trust a folder once and experts can read, write and run things in it — in
  Chat and in Build alike. Every model gets the same tools, local or
  at a provider, in a text protocol every model can follow: one verb on a line
  of its own, and the result comes back.
- **In the folder:** LIST, READ and WRITE. READ reads a PDF, a Word file or a
  spreadsheet as its text, a web page as its source, and a picture as a
  picture to a model that sees one. Every path is resolved inside the project root; absolute paths, `..`
  and symlinks that escape it are refused.
- **Commands:** RUN, in the platform's shell or one it names — `RUN zsh:`,
  `RUN powershell:`, `RUN cmd:`, `RUN fish:`. PYTHON, with a block of code run
  on Crucible's own Python in the project. GIT, and GH when GitHub's command
  line is installed: `GIT: log --oneline -5`, `GH: pr list`. START leaves a
  program running — a development server — and LOGS and STOP read and end
  it; what is running is listed in Settings, Tools and stopped when Crucible
  closes.
- `RUN` is worth being exact about: a command starts in the project root and
  is otherwise a command, and GIT, GH, PYTHON and START are commands by other
  names. Real confinement means a sandbox, which is not here yet.
- **The web**, with the switch on: SEARCH, through DuckDuckGo with no key,
  or Wikipedia, searxng or Brave; and FETCH, which reads a page as its text,
  rendered in a headless Chrome, Chromium or Edge when the machine has one
  and through curl when it does not.
- **This computer**, with its own switch, off by default: SCREENSHOT, CLICK,
  MOVE, TYPE, KEY and SCROLL, done with `screencapture` and `osascript` on a
  Mac, `xdotool` and a screenshot tool on Linux, PowerShell on Windows. A
  screenshot goes to a model that sees pictures as a picture; with
  `tesseract` installed, a local model that reads text only gets the words
  off it. Nothing bounds a click the way a folder bounds a file, which is why
  it is a switch of its own. On a Mac the first screenshot and the first
  click ask for Screen Recording and Accessibility.
- A picture an expert took or looked at is in the transcript and the journal
  as something to open.
- Auto off (the default) shows you each edit before it lands, in Chat and
  Build alike, and the switch sits beside the box on both screens.

![The question Crucible asks once per folder](docs/images/trust.png)

### Run on your hardware

- Compute runtimes — CUDA, Vulkan, Metal, CPU — are installed from the settings
  screen: downloaded where a build is published for your platform, compiled
  here where it is not. Either way against the same llama.cpp the program was
  built from, because a backend built against another one crashes on the first
  tensor.
- GGUF models, and on Apple Silicon **MLX** models too: a folder of
  `.safetensors` is run by MLX's own server, from the Python environment the
  fine-tuner uses, on this machine only. The models folder is read four levels
  deep, so LM Studio's `publisher/model/` layout, a folder of MLX folders and
  Hugging Face's own cache all work as they are.
- A downloaded CUDA backend needs only an NVIDIA driver — no toolkit. The two
  libraries it needs that the driver lacks, NVIDIA's CUDA runtime and cuBLAS,
  are fetched from NVIDIA with it (a few hundred megabytes) unless the machine
  already has them.
- Multiple GPUs: `auto`, `even`, `priority` (an order you arrange) or `single`.
- Optional: keep every layer on the GPU, and refuse models that will not fit in
  video memory.
- A change in Settings → Hardware lets go of whatever it affects, and the next
  prompt loads it the new way.

![Runtimes: what is installed, what it found, and what it would take to add more](docs/images/runtimes.png)

### Reach further, when you choose to

- A seat does not have to be a file. It can be answered by a provider: Claude
  through Anthropic's API, or anything that speaks the OpenAI one -- OpenAI,
  Gemini, DeepSeek, Moonshot's Kimi, Cloudflare Workers AI, OpenRouter, Groq --
  and equally a llama.cpp, Ollama or LM Studio server on your own network.
- Use one as a specialist, or as the default expert: the large general model
  that catches what the local ones could not place.
- Nothing is sent anywhere until you add a provider and point a seat at it,
  and a seat that leaves the machine is marked as one. A key can be read from
  an environment variable rather than written to the config file.
- The delegator stays local. It is asked for a probability per expert, which
  is not a thing a provider's API will give.

### Keep the work

- History is per project, and reopening a conversation hands the exchanges back
  to the expert rather than starting cold.
- The panel on the right lists recent conversations across every project,
  named for what they are about, and the projects you have opened; choosing
  one opens it and carries on from where it stopped.
- **New chat** needs no project: each one gets a scratch folder of its own
  under `~/Crucible/Scratchpad` for whatever it makes. To work on a folder of
  yours, open it as a project from the same panel.
- The top bar shows the folder the chat works in; click it to open that
  folder in your file manager.
- A conversation that outgrows the context window rolls, truncates its middle,
  or stops — your choice — and says so in the transcript when it drops
  anything.

![Every cook and conversation this project has had](docs/images/history.png)

___
## Configuration

Everything is editable from the settings screen, and every change applies as
it is made: models directory, a model for each seat, providers, sampling,
hardware, runtimes, tools, and how a build is run — who plans it, the model
for new agents, whether to show the plan first, commits, rounds per task. The
file behind it is `~/.config/crucible/config.json`, and it can be edited by
hand.

Ctrl+1 to 4 switch between Chat, Build, Create and History; Ctrl+, opens
Settings; Ctrl+U attaches files.

___
## Build from source

A C++20 compiler, CMake ≥ 3.24, git, and on Linux WebKitGTK's development
files — `libwebkit2gtk-4.1-dev` on Debian and Ubuntu, `webkit2gtk4.1-devel` on
Fedora. macOS and Windows provide their own webview. llama.cpp, webview and
nlohmann/json are fetched and pinned automatically. No Python is needed to build
or to run: Crucible fetches its own on first start, or with
`crucible --install-python`. The tests start the orchestrator, so running them
wants a Python 3.10 or newer on PATH (or named by `CRUCIBLE_PYTHON`).

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo cmake --install build --component crucible
```

Pass `--component crucible`: a plain install would also drop llama.cpp's own
libraries loose into the prefix.

___
## Contributing and AI policy

I am open to AI and agentic coding, but the code written needs to follow
specific guidelines:

1. MUST be human readable, acceptable variable/function names.
2. Easily traceable, following a good program flow.
3. Contributor MUST look at/document code and code changes. You need to
   understand the code that is being written.

Every push is built and tested on Linux, macOS and Windows.

___
## License

MIT — see [LICENSE](LICENSE). What is compiled into the binary is listed in
[THIRD_PARTY.md](THIRD_PARTY.md) with pinned versions, and `crucible
--licenses` prints the notices from the program itself. Most of it is MIT;
the JetBrains Mono typeface is under the SIL Open Font License, which is why
the notices travel inside the binary rather than beside it.

Crucible's own Python is not shipped with it either: on first start it fetches
a pinned CPython 3.12 build from the python-build-standalone project, checks
it against the SHA-256 compiled into Crucible, and unpacks it into its data
folder. CPython is under the PSF License; THIRD_PARTY.md has the rest.

The fine-tuner is separate and is not shipped with Crucible: pip fetches it
onto your machine when you ask for it. It is mostly Apache-2.0, BSD and MIT,
and on an NVIDIA machine PyTorch brings NVIDIA's CUDA libraries, which are
proprietary. THIRD_PARTY.md names them. `--no-trainer` declines the whole
thing, and the CPU and Apple Silicon builds pull in nothing proprietary.

Model weights are covered by none of it. Crucible ships no models and downloads
none; whatever GGUFs you put in your models directory carry their own licenses,
which are between you and whoever trained them.

The software is provided "as is", without warranty of any kind. You are
responsible for what your models read, write and run on your machine.
