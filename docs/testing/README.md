# Testing Crucible

How to check a change, and how to check a release. Written for a Linux machine
with a GPU; the same steps work on macOS and Windows.

## Every change

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCRUCIBLE_BUILD_TESTS=ON
cmake --build build -j$(nproc)
build/bin/crucible_tests                                   # C++ and the page (JavaScriptCore)
python3 -m unittest discover -s scripts/orchestrator/tests # the orchestrator
bash tests/test_install.sh && bash tests/test_uninstall.sh # the installers
```

All three must pass. CI runs them on Linux, macOS and Windows for every push;
a failed C++ test shows on the commit as an annotation (case, file, line).

Optional, with a vision model and its `mmproj` file in the same folder:

```sh
CRUCIBLE_VISION_MODEL=/path/to/SmolVLM-256M-Instruct-Q8_0.gguf build/bin/crucible_tests
```

## Before a release: the budgeting app

The acceptance test. Use the best model the machine runs: a 30B coder
(Qwen3-Coder-30B-A3B) or a provider key. A 14B coder works but is slow and
needs more nudging.

1. **Fresh start.** Move `~/.config/crucible` and `~/.local/share/crucible`
   aside, start Crucible. It fetches its Python, the runtimes, the kit
   (gh, Node, a headless browser, RapidOCR), and a model when there is none.
   Nothing to click.
2. **Build.** In Build: *I need a budgeting software that gives me charts of
   my gross and net revenue.* Reply *go* to the plan.
   - The agents are listed under the experts in the side menu.
   - The status line and the working agent are orange.
   - The bottom right shows the model in memory (and the provider's cost,
     with a key).
3. **Use it.** Preview: type two numbers, press Save, see two bar charts.
4. **Change a color.** *Change the color from red to blue* (whatever color it
   came out). One task, no plan question; the preview updates.
5. **Move a button.** *Move the Save button farther away from the side.*
6. **Ship it.** Make it an app. Open it from the applications menu, type
   numbers, Save, close it, open it again: the numbers are still there.
7. **Tune by eye.** Preview, Pick an element, change padding or color,
   Apply to source.

Note what failed and how long each step took.

## Before a release: the Models tab

1. **Search.** Models, type a name. Best match puts the used repositories
   first; Most likes and the others reorder it.
2. **Choose.** Open one: its quantizations, each marked Fits on GPU, Partial
   offload, CPU only or Too large, the suggested one starred.
3. **Download a small one** (SmolLM2-135M-Instruct, 270 MB). The bar fills,
   the file lands in `models/<author>/<repo>/`, the option says Downloaded.
4. **Cancel and resume.** Start a large one, Cancel: the part file goes.
   Start it again and close the window halfway; open Crucible and download
   it again: it carries on from where it stopped, and finishes.
5. **MLX** lists repositories too; on Linux and Windows each says Mac only.

## Before a release: Bench

1. **Run.** Bench, pick a small model (LFM2.5-1.2B), Quick, every benchmark.
   A bar fills per benchmark, with how long is left. The first run fetches
   the rows from Hugging Face; later ones read them from
   `bench/rows/`.
2. **Read it.** The result opens to each benchmark with its margin of error,
   and a wrong answer shows what the model said and what was read from it.
3. **Compare.** Compare two or three results: the spider chart, a color
   each, and the numbers under it.
4. **Stop.** Start another run and Stop it halfway: what finished is kept,
   marked stopped, and a benchmark that did not finish is not counted.
5. **Post** (on a test copy of the board, or delete the issue after): the
   dialog shows exactly what is sent. With `gh` signed in it opens an issue;
   the board's Action comments with the scores and `board.json` takes it.

## Before a release: condensing and the code editor

1. **Condense.** Create, Condense a model, pick a Q8: every size is listed
   with how it fits. Condense to Q4_K_M, then Test it. Then IQ2_XXS: it says
   it is running the model over sample text, then condenses. A second
   sampled size of the same model skips that step.
2. **Edit.** Open a project, Build, Code. Open two files, change one (the
   tab and the tree mark it), Ctrl+S. Ctrl+P finds a file by a few letters;
   Ctrl+Shift+F searches the project. While an agent writes a file that is
   open and changed, the editor says so and keeps your text.

## Before a release that moves llama.cpp: an update

The first start after an update replaces every runtime. Start the new build
on a data folder the way the last release left it:

1. Unpack the last release's `runtime-*-<its tag>.tar.gz` for this machine
   into `~/.local/share/crucible/runtimes` under a scratch `HOME`, with
   NVIDIA's libraries in `runtimes/cuda`.
2. Write `runtimes/manifest.json` naming that tag for each backend:
   `{"cpu": {"llama_tag": "b10678"}, "cuda": {"llama_tag": "b10678"}}`.
3. Start the new build with that `HOME`. In `crucible.log`, every runtime
   says `loaded ... backend` after its download, with no restart.

`crucible --install-runtimes` does the same without a window.

## Where to look when something goes wrong

| what | where |
|---|---|
| a build, step by step | `~/.local/share/crucible/projects/<project>/cooks/*.json` |
| what a failed EDIT tried | that step's `detail` |
| logs | `~/.local/share/crucible/crucible.log`, `crash.log` |
| chats' folders | `~/.crucible/Scratchpad` |
| settings | `~/.config/crucible/config.json` |

## Without a window

```sh
build/bin/crucible-smoke - model.gguf "a question"
build/bin/crucible-smoke --build model.gguf "a directive" "a question asked while it builds"
```

Exits non-zero when the turn or the build failed.

## Releasing

1. Set the version in `CMakeLists.txt`, `packaging/macos/dmg.sh`,
   `packaging/windows/crucible.iss`, `CHANGELOG.md` and `README.md`
   (`tests/test_install.sh` checks they agree).
2. Push to `main` and wait for CI to pass on all three systems.
3. Tag and push the tag: `git tag v0.9.5 && git push origin v0.9.5`.
   The installers workflow builds the three installers and, in the same run,
   the prebuilt runtimes. The runtimes go on a draft release as they finish,
   and the release is published only when the installers are added to it, so
   an install never finds a release without its runtimes.

## Seen on 2026-10-10

macOS, M4, 24 GB, no provider. The scenario got as far as step 3.

- **Qwen2.5-Coder-14B (GGUF).** 5 to 15 minutes a task. Fixed along the
  way: plans split into many tasks when one was asked for, a "check" written
  as a sentence and run as a command, edits that copied READ's line numbers,
  `RUN: RENDER:`, and new lines written after `>>>>>>> REPLACE` read as a
  deletion. Still weak: edits from memory instead of the file, and sometimes
  writes over a whole file.
- **gpt-oss-20b (MLX).** Plans in 30 seconds. Fixed: MLX models got no
  repetition penalty, so it looped; two EDITs in one reply. Still open: a
  WRITE sometimes arrives with no content, and it reads files with
  `RUN: sed -n` rather than READ.

Next: the whole scenario on a 30B coder or with a provider key.

## Seen on 2026-10-21

Linux (Arch, KDE Plasma on Wayland), an RTX 4070, 5060 Ti and 3060, 0.9.2.

- **gpt-oss-20b (GGUF, CUDA).** The budgeting app planned in 18 seconds and
  built in 2 minutes: 4 tasks, 3 files, the README task routed to Writing.
  Two rows typed into the preview showed in the table and the chart. Still
  open: "make the header blue" looped on `RUN: sed -n` and a grep for
  "green" until its rounds ran out; naming the file and the property worked.
- **LFM2.5-1.2B as delegator** sent a CSS change to the Writing seat.
- **Qwen3.8-27B (Q8_0)** split across the three cards with Dedicated VRAM
  only, and answered.
- **Updating from 0.9.1**, after the release: the first start loaded no
  runtime until a restart, in the window and in the installer. Every check
  above ran on fresh installs, which cannot show it. Fixed in 0.9.3, and the
  update test above is what finds it.

## Seen on 2026-10-23

Linux (Arch, KDE Plasma on Wayland), an RTX 4070, 5060 Ti and 3060, 0.9.4.

- **The Models tab** found, sorted and opened repositories from Hugging Face.
  A 258 MB GGUF downloaded at 73 MB/s; a 3.4 GB one, canceled, then stopped
  halfway by closing the window, carried on at the next start and finished
  with Hugging Face's SHA-256.
- **Picking an expert in the box** loaded it at once, kept it loaded between
  prompts with the delegator on demand, and freed it for the delegator when
  the box went back to Delegator.
- **The chat box** held still while typing during download progress; before,
  every push moved its top edge by about 2 pixels and back.

## Seen on 2026-10-26

Linux (Arch, KDE Plasma on Wayland), the 5060 Ti and 3060 (the 4070 left
alone), 0.9.5.

- **Condense.** LFM2.5-1.2B Q8_0 to Q4_K_M from Create: every size measured
  at once (the 27 GB Qwen3.8 Q8_0 too), the copy made in 3 seconds (697 MB),
  and it answered "Paris" in Test. flint's eleven sizes measure in 342 ms.
- **Bench, Quick, all nine.** The condensed LFM2.5 scored 60.7 overall in 9
  minutes at 356 tokens a second, every skill present. The first prompts
  showed "Answer: B" as their example, and the 1.2B answered B to nearly
  everything (30 of 50 on ARC); the format is now `Answer: $LETTER`, and ARC
  went from 64% to 86%.
- **A dataset page that would not come** (TruthfulQA, once) left that
  benchmark out without a word on screen. Pages are fetched before any model
  loads now, tried again over a minute and a half, and a result says what it
  missed.
- **Code.** A project opened from the Projects list; the tree, tabs, line
  numbers and colors drew. The colored layer ran past the bottom of the box
  on a long file (the numbers beside it set its height); fixed.
- **Create's Started and Finished** had been blank since 0.9.4.
- **The smallest sizes.** LFM2.5-1.2B Q8_0 to IQ2_XXS on the 4070 in 31
  seconds: 98 chunks of WikiText measured, 365 MB written. IQ1_M of it took
  7 seconds with the matrix kept. Both are badly hurt at 1.2B (IQ2_XXS knew
  Paris, not the Seine), which is what those sizes do to a small model.
  gpt-oss-20b measured on the 5060 Ti with every expert seen, and came out
  at 12.0 GB of 12.1: its 2880-wide rows do not divide into the 256-wide
  blocks, so llama.cpp falls back to IQ4_NL for most of it. The tab now
  offers a size only when the dry run says it shrinks.
- **The first sample pass** missed the last layer: llama.cpp takes only the
  last token through it unless every token's output is asked for. Fixed by
  asking.
- **An update over 0.9.4's runtimes**, whose NVIDIA libraries (CUDA 13.3.1)
  have no release file beside them: `--runtime-status` says CUDA active, two
  devices. Before the fix, CUDA would have been off until 1.4 GB came down
  again.
- **A split across two cards** (the 4070 and the 3060, Vulkan hidden):
  gpt-oss-20b went 4.7 GB onto the 4070, which also drives the desktop, and
  6.2 GB onto the 3060, every layer on a card, with the compute buffers
  exactly what the dry run said. It answered 17 times 23 in 10 seconds,
  load included.
- **Claude Haiku 5.5 through the Anthropic provider**, the key only in the
  environment: the model list (14), a reply, thinking shown, a picture read,
  READ in a trusted folder, and a build that wrote hello.html, for about half
  a cent in all. Found and fixed: a thinking reply's rate read 20,000 tokens
  a second, the delegator was loaded after every reply and to name chats
  while a provider's expert was chosen, and a lone `<br>` before a tool call
  was kept as the reply's first line.
- **Builds with gpt-oss on two cards** (the 4070 and the 3060, for the README
  pictures). At high effort in its 8K context it reached for apply_patch,
  then re-read files it had just read and wrote `...` into app.js; at medium
  effort the same change was made and committed. Its architect twice wrote a
  whole plan without the last brace. LFM2.5 as delegator matched no seat to
  "HTML editing", so the build made one with the config's worker model, the
  27B Qwen, which does not fit on two cards; a check task by Programming
  finished the change. Fixed along the way: the apply_patch hint, the plan
  reader, the tune panel's 19.200001px, and the bench clock that stood still
  while pages were fetched.
