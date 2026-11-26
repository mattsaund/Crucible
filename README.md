```
   ⠀⠀⠀⠀⠀⠀⢱⣆⠀⠀⠀⠀⠀⠀
   ⠀⠀⠀⠀⠀⠀⠈⣿⣷⡀⠀⠀⠀⠀
   ⠀⠀⠀⠀⠀⠀⢸⣿⣿⣷⣧⠀⠀⠀
   ⠀⠀⠀⠀⡀⢠⣿⡟⣿⣿⣿⡇⠀⠀
   ⠀⠀⠀⠀⣳⣼⣿⡏⢸⣿⣿⣿⢀⠀   C R U C I B L E
   ⠀⠀⠀⣰⣿⣿⡿⠁⢸⣿⣿⡟⣼⡆   An all in one local AI workshop.
   ⢰⢀⣾⣿⣿⠟⠀⠀⣾⢿⣿⣿⣿⣿
   ⢸⣿⣿⣿⡏⠀⠀⠀⠃⠸⣿⣿⣿⡿
   ⢳⣿⣿⣿⠀⠀⠀⠀⠀⠀⢹⣿⡿⡁
   ⠀⠹⣿⣿⡄⠀⠀⠀⠀⠀⢠⣿⡞⠁
   ⠀⠀⠈⠛⢿⣄⠀⠀⠀⣠⠞⠋⠀⠀
   ⠀⠀⠀⠀⠀⠀⠉⠀⠀⠀⠀⠀⠀⠀
```

**Crucible** builds software from a sentence, answers with the right expert
for each question, and gets, makes and measures the models it runs. Local
models do what they can, and frontier models do the heavy lifting once you
add a key. Nothing leaves the machine until you say so.

Website: https://msaunders.dev/crucible/

![One prompt: Programming writes the page and the note, and both land on the shelf](docs/images/ask.png)

___
## Install

From the [releases page](https://github.com/mattsaund/Crucible/releases):

| | | |
|---|---|---|
| macOS | `Crucible-macOS.dmg` | drag into Applications |
| Windows | `Crucible-Setup.exe` | run it, no administrator needed |
| Linux | `Crucible-x86_64.AppImage` | `chmod +x`, run it |

Not signed with a paid certificate. On Windows, click **More info → Run
anyway**. On a Mac, open it once, then **System Settings → Privacy & Security →
Open Anyway**.

**The first start fetches the rest**, with progress shown and nothing to click:

- Crucible's own Python (no system Python needed or touched)
- the compute runtimes the machine can use (CPU, CUDA, Metal, Vulkan)
- the programs experts use, like GitHub's CLI, Node.js, a headless browser,
  Git on Windows and a text reader for pictures on Linux
- with no model at all, an open coding model sized to the machine (Qwen,
  Apache-2.0), seated as **Programming**
- the fine-tuner's environment

**Or build from source**

```sh
curl -fsSL https://raw.githubusercontent.com/mattsaund/Crucible/main/install.sh | bash
```

```powershell
irm https://raw.githubusercontent.com/mattsaund/Crucible/main/install.ps1 | iex
```

The options are `--prefix DIR`, `--jobs N`, `--check`, `--no-deps`,
`--no-trainer`, `-y` and `--uninstall`. Run from inside a clone, it builds that
clone.

**Or use it in the terminal.** `cruciblecli` is a separate download that does
everything the app does, with no window. One line, then type `cruciblecli` in
any folder.

```sh
curl -fsSL https://raw.githubusercontent.com/mattsaund/Crucible/main/install-cli.sh | sh
```

```powershell
irm https://raw.githubusercontent.com/mattsaund/Crucible/main/install-cli.ps1 | iex
```

It shares its settings, models and chats with the app, if you have both.
`/update` updates it, and adding `-s -- --uninstall` to the `sh` line removes it.

**Updating:** Settings → About → **Update now**, or run the installer again.
Crucible checks GitHub once a day and says when there is one, and with
**Pre-releases** ticked there it offers pre-releases and betas too. Config,
models and history are kept. Versions are tagged `v1.0.0` (`--branch v1.0.0`
builds one from source), and what changed is in [CHANGELOG.md](CHANGELOG.md).

**Uninstall:** `crucible --uninstall`. It asks before deleting models. If the
program will not run, use one of these.

```sh
curl -fsSL https://raw.githubusercontent.com/mattsaund/Crucible/main/install.sh | bash -s -- --uninstall
```

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/mattsaund/Crucible/main/install.ps1))) -Uninstall
```

___
## What it does

### Ask for anything

- **One box, one loop.** Ask for anything. The expert works on it with the
  tools until it is done, and the delegator brings in another expert when the
  next step needs one.
- A question is answered. Ask for software, like "a budgeting app that charts
  my gross and net revenue", and the expert reads, writes and runs what it
  takes, an action at a time, until the app is there.
- **The expert can change mid-prompt.** When the next step wants another kind
  of expertise, the delegator picks who takes it, and that expert carries on
  with the same conversation.
- Each action is shown above the reply, with the diff it made or the output it
  printed. While it works, the reply says what it is doing: reading the
  prompt, thinking, the file it is writing as it writes it, or a command and
  what it prints.
- **A change is a sentence.** "Make the button blue" is made and shown.
- Every prompt is a turn of the conversation, so "now add dark mode" knows the
  project.

### Code

- **An editor** beside the conversation, with tabs, a file tree, find and replace,
  search across the project, Ctrl+P to open a file by a few letters, and the
  terminal under it.
- **A real terminal.** Your own shell in the project, in as many tabs as you
  like, with colors, line editing and full-screen programs like vim and top.
- A file an expert changes while it is open reloads. Unsaved work is never
  written over.

![The code editor, with the project's files beside it](docs/images/code.png)

### Preview, tune, ship

- **The shelf** on the right of the chat lists what the experts made there,
  and keeps it with the chat. Code is a **Build**, which opens with Code,
  Source, Preview and Terminal. Anything else opens in place: a PDF in the
  window's own reader, pictures, recordings and films, documents, tables and
  archives. **Download** saves it to your Downloads folder and **Save as**
  where you choose, and the build goes as one .zip.
- **Preview** shows the page being made and reloads as an expert writes it.
- **Pick an element**, then nudge padding, margin, fonts, colors and more.
  **Apply to source** hands the change to an expert, who writes it into the
  stylesheet.
- **Make it an app**, a program of its own in Applications, the Start menu or
  the applications menu. It runs without Crucible and keeps its own data.
- **Source** is git, with changes, diffs, commits, pull, push, history and CI
  runs. **Publish to GitHub** and **Ship** a release with `gh`.

![The preview, with an element picked and its dashboard open](docs/images/preview.png)

### Experts

- **A roster of experts.** A small router model, the delegator, sends each
  prompt to the expert that fits. It scores the roster, so it cannot name one
  that does not exist.
- Pin a prompt to one expert from the box, or with `/name`. Its model loads
  at once and the delegator steps aside.
- **Effort and thinking follow the model.** The effort menu offers exactly the
  levels the chosen model takes, and a Thinking switch shows for a model that
  can think or not.
- Replies show Markdown, code and math, typeset from LaTeX.
- **Providers** are Anthropic and anything that speaks the OpenAI API, like
  OpenAI, Gemini, DeepSeek, Kimi, OpenRouter, Groq or a server on your network.
- The foot of the side menu shows the models in memory, and the frontier
  model's tokens, cost at list prices, rate limits and context.

![The roster: each seat with a description and a model](docs/images/experts.png)

### Tools

Every model, local or not, gets the same tools in a trusted folder:

- LIST, READ, FIND, WRITE and EDIT **files**, PDFs, Word, sheets and pictures
  too
- RUN **commands** in any shell, and PYTHON, GIT, GH, and START, LOGS and STOP
  for servers
- SEARCH **the web** (DuckDuckGo, no key) and FETCH pages in a headless browser
- RENDER an HTML page or SVG to PDF or PNG
- TOOL calls any MCP server added in Settings
- SCREENSHOT, CLICK, MOVE, TYPE, KEY and SCROLL on **this computer**, off by
  default
- Edits wait for your OK unless Auto is on. RUN is not a sandbox.

![The question Crucible asks once per folder](docs/images/trust.png)

### Attach

- Attach documents, code, pictures and folders with **+**, Ctrl+U, or by
  dropping them on the window. Paste a picture with Ctrl+V.
- PDFs, Office files, EPUB and text are read into the prompt. Pictures go to
  models that see, and the others get the words in them.

### Models

- **Download from Hugging Face** in **Models**. Search, sort by best match,
  downloads, likes or recently updated, pick GGUF or MLX and a quantization,
  and see whether it fits on the cards, runs partly on the CPU, or is too
  large. Downloads carry on after a dropped line or a restart and are checked
  against Hugging Face's checksum.
- Each result shows its own picture when its card names one, and otherwise
  its maker's logo, the original maker's for a repack.
- GGUF everywhere, MLX on Apple Silicon, where an MLX model can be the
  delegator too. LM Studio and Hugging Face folders work as they are.
- Models that see pictures work when their `mmproj` projector sits beside them.
- Models stay loaded while they fit, so a follow-up question loads nothing.
  When one has to make room, the one used longest ago goes, and one in use is
  never freed.

![The Models tab: a repository's quantizations, each marked by how it fits](docs/images/models.png)

### Create

- **Fine-tune an expert.** Pick a base model and data, train it on your card
  (LoRA, or QLoRA with CUDA), test it and keep it. A tenth of the data is held
  out and scored before and after, so you see what training did.
- **Condense a model.** A Q8 that does not fit becomes a Q4 that does, and the
  largest models go down to IQ2 or IQ1, made after a pass over sample text that
  finds the weights that matter most. Every size is measured first and marked
  by how it fits.

![Condensing: every size, measured, and how each fits](docs/images/condense.png)

![The experts made here](docs/images/create.png)

### Bench

- **Bench opens on the [public board](https://github.com/mattsaund/crucible-bench).**
  The best local models come first, each with a Download that opens it in
  Models, then the spider chart, then every result there is, searchable.
- **Add benchmark** runs your models through nine benchmarks in seven skills
  (MMLU-Pro, ARC-Challenge, GSM8K, MATH-500, HumanEval, MBPP, IFEval,
  HellaSwag and TruthfulQA). Quick asks 50 items of each, Standard 200, Full
  every one.
- Every Crucible asks the same items the same way, so a score from one machine
  stands beside a score from another.
- **Spider charts** compare up to three models. A result opens to every
  benchmark, its margin of error, and the answers that were wrong.
- **Post** a result from your GitHub account. A result is names and numbers,
  nothing typed, and its model has to be a public file on Hugging Face.
- **Bench vs base** puts a fine-tune beside the model it was made from.
- HumanEval and MBPP run the code a model writes on this machine, with time
  and memory limits. It is not a sandbox.

![Bench: the board's best models, a spider chart, and every result](docs/images/bench.png)

### Serve

- **Serve a model to other programs**, the way LM Studio does. Turn it on in
  Settings, Server, pick the delegator or any model here, and point any OpenAI
  client at the address it shows.
- It answers on this machine only, or on your network when you say so, over
  IPv4 and IPv6. An API key is optional.
- Tools, pictures, JSON answers and embeddings work as OpenAI's clients
  expect. A provider's model can be served too, to other computers only with
  a key.
- HTTPS when you turn it on, with your own certificate or one Crucible makes
  for this machine and keeps. Settings shows its SHA-256 to check it by.
  A certificate renewed in place is served as soon as it changes, and
  Crucible's own follows the machine to a new network.
- Chats keep working while it serves. A request waits its turn.
- It listens only while it is on.

![The local server over HTTPS: its certificate's SHA-256, its addresses on this machine and the network, and the last requests](docs/images/server.png)

### Hardware

- Runtimes (CPU, CUDA, Metal, Vulkan) install from Settings, downloaded or
  compiled. A downloaded CUDA runtime needs only the NVIDIA driver, 580 or
  newer.
- **A newer llama.cpp without a new Crucible.** Settings, Runtimes lists the
  versions published for this Crucible, and Use restarts on one. With
  Auto-update on, Crucible fetches the newest once a day and runs on it from
  the next start, the way LM Studio keeps its runtimes current.
- MLX, on Apple silicon, updates from the same page.
- Several GPUs work as one, split automatically, evenly, by priority, or kept
  to a single card.

![Runtimes: llama.cpp's version, kept up to date, and CPU, CUDA and Vulkan, all three active](docs/images/runtimes.png)

### History

- History is per project, in the side menu, and a reopened chat carries on
  where it stopped.
- A chat with no project gets a folder of its own in `~/.crucible/Scratchpad`
  (hidden). The side menu lists recent chats and projects.

![Every chat of a project](docs/images/history.png)

### In the terminal

- **`cruciblecli` opens in the folder your terminal is in** and does everything
  the app does, over SSH too.
- A box at the bottom, with what it is doing in one line right above it and the
  expert that has the prompt right below it.
- Code changes are drawn as diffs and commands as they run, with their output.
  The thinking sits among them, collapsed until you type `/showthinking`.
- `/settings` has every page the app has, and `/models` searches Hugging Face
  and downloads what fits your cards.
- `/effort`, `/expert`, `/resume`, `/files`, `/git`, `/preview`, `/create`,
  `/bench`, `/server` and the rest. `/help` lists them.
- `!command` runs a command, `@file` attaches a file, and `/name prompt` sends
  a prompt straight to one expert.
- `cruciblecli -p "prompt"` answers once and exits, for scripts.

![Crucible in the terminal: an edit as a diff, the command it ran, and the expert that did it](docs/images/terminal.png)

___
## Settings and keys

Everything is in Settings and applies at once. The file is
`~/.config/crucible/config.json`, or `%APPDATA%\crucible\config.json` on
Windows.

- Ctrl+1 to 4 open Chat, Create, Models and Bench.
- Ctrl+, opens Settings, and Ctrl+U attaches.
- In the editor, Ctrl+P opens a file, Ctrl+Shift+F searches the project, Ctrl+S
  saves, Ctrl+F finds, Ctrl+H replaces and Ctrl+G goes to a line.

___
## Build from source

C++20, CMake 3.24+, git, and on Linux WebKitGTK (`libwebkit2gtk-4.1-dev` on
Debian and Ubuntu, `webkit2gtk4.1-devel` on Fedora). Dependencies are fetched
and pinned. The tests want Python 3.10+ on PATH (or `CRUCIBLE_PYTHON`).

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo cmake --install build --component crucible
```

___
## Contributing and AI policy

I am open to AI and agentic coding, but the code written needs to follow
specific guidelines:

1. MUST be human readable, acceptable variable/function names.
2. Easily traceable, following a good program flow.
3. Contributor MUST look at/document code and code changes. You need to
   understand the code that is being written.

Every push is built and tested on Linux, macOS and Windows.
[docs/testing](docs/testing/README.md) says how to test a change or a release.

___
## License

MIT, see [LICENSE](LICENSE). What is compiled in is listed in
[THIRD_PARTY.md](THIRD_PARTY.md), and `crucible --licenses` prints the notices.

Some things are fetched rather than shipped, each under its own license.
Crucible's Python (PSF License), the fine-tuner (mostly Apache-2.0, BSD and
MIT, with NVIDIA's proprietary CUDA libraries on NVIDIA machines), the experts'
programs, the starter model (Apache-2.0), the benchmarks' questions and every
model's weights.

Provided "as is", without warranty. You are responsible for what your models
read, write and run.
