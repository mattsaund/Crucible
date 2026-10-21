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
3. Tag and push the tag: `git tag v0.9.3 && git push origin v0.9.3`.
   The installers workflow builds the three installers and publishes the
   release.

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
