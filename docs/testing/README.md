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
3. Tag and push the tag: `git tag v0.8.9 && git push origin v0.8.9`.
   The installers workflow builds the three installers and publishes the
   release.

## Seen on 2026-10-10

macOS, M4, 24 GB, Qwen2.5-Coder-14B, no provider. Each task took 5 to 15
minutes. Fixed along the way: plans split into many tasks when one was asked
for, a "check" written as a sentence and run as a command, edits that copied
READ's line numbers, `RUN: RENDER:`. Still weak at that size: the model edits
from memory instead of the file, and sometimes writes over a whole file. Worth
re-running on a bigger model.
