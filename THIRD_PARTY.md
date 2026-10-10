# Third-party licenses

Two separate things: what the `crucible` binary is built from, and what it
installs for you afterwards. The first is below and is small; the second is
the training environment, further down, and is neither distributed with
Crucible nor MIT.

## Compiled into the binary

Fetched at build time by `cmake/CrucibleDependencies.cmake`, pinned to exact
tags. No third-party source is vendored into this repository.

`crucible --licenses` prints the full notices for everything below, so they
travel with the program rather than only with the source — which is what
JetBrains Mono's license requires, since the font is inside the binary.

| project | version | license | what it does here |
|---|---|---|---|
| [llama.cpp](https://github.com/ggml-org/llama.cpp) | `b10678` | MIT | loads GGUF models and runs inference |
| [nlohmann/json](https://github.com/nlohmann/json) | `v3.12.0` | MIT | reads and writes the config file, and is the API's encoder |
| [JetBrains Mono](https://github.com/JetBrains/JetBrainsMono) | `2.304` | SIL OFL 1.1 | the interface typeface, compiled in rather than looked for |
| [webview](https://github.com/webview/webview) | `0.12.0` | MIT | hosts the interface in the platform's own webview |
| [miniz](https://github.com/richgel999/miniz) | `3.1.2` | MIT | opens the zip inside a `.docx`, `.xlsx`, `.pptx` or OpenDocument file attached to a prompt, and inflates a PDF's streams |

One of these is not MIT, which is worth stating plainly rather than leaving a
reader to notice. JetBrains Mono is under the SIL Open Font License, which
requires its copyright notice and license to accompany the font wherever the
font goes. The font goes inside the binary — it is handed to the interface as
a data URI — so the notice does too.

Dear ImGui and GLFW were here until the interface moved to the platform's own
webview. Neither is linked any more and both are gone from the build.

## The window itself

The table above is what Crucible *carries*. The window it opens is the
operating system's, and that is not distributed here either:

| platform | what draws the window | license |
|---|---|---|
| Linux | WebKitGTK (`libwebkit2gtk-4.1`), installed by your package manager | LGPL-2.1 and BSD-2-Clause |
| macOS | WKWebView, part of the system | Apple's |
| Windows | WebView2, from the Edge runtime Windows ships | Microsoft's |

Crucible links these dynamically and ships none of them, so no copy of theirs
travels with the program. On Linux this is a real dependency rather than a
detail: the installer checks for it and says which package provides it, and
the AppImage refuses to build without it.

## What it talks to the network with

Every request Crucible makes -- the version check, a web search, a download
from Huggingface or of a runtime, a prompt sent to a provider -- is made by
running `curl`, the one already on the machine. It is not distributed with
Crucible and nothing of it is linked in. On Windows 10 and later and on macOS
it ships with the system; on Linux the installer checks for it.

Request headers and bodies are handed to it in a file only this user can read
rather than on its command line, so a key is never somewhere another process
on the machine can see it.

## What an attachment is read with

The documents attached to a prompt are read by Crucible's own code: miniz
above for the formats that are zip files, and readers in `src/tools/` for
PDF, RTF and the Office formats from before 2007. Two programs are used
instead when the machine already has them, because they read more of the
files in the wild: `pdftotext` (Poppler, GPL) for a PDF, and on Linux
`catdoc` (GPL) for an old Word, Excel or PowerPoint file that the built-in
reader finds nothing in -- `textutil`, part of macOS, does that job there.
Each is run as a separate program, the way `curl` is; none is distributed
with Crucible or linked into it.

## What an expert's tools run

The tools an expert reaches for beyond the project folder are programs the
machine has or does not, run the way `curl` is -- started as a separate
process in the project folder, their output read -- and none is distributed
with Crucible or linked into it. What the machine lacks of a few of them,
Crucible fetches on its first start from where each is published, into its
own data folder (`kit/`), and runs from there: the kit, in the second table.

| tool | program | license | where it comes from |
|---|---|---|---|
| `GIT:`, commits after a build's tasks, the Source panel | [git](https://git-scm.com) | GPL-2.0 | your package manager, Xcode's tools, Git for Windows |
| `GH:`, Publish to GitHub, Release, CI runs | [gh](https://cli.github.com) | MIT | installed, or fetched into the kit |
| `FETCH:` and `RENDER:` | Chrome, Chromium, Edge or Brave run headless, or Chrome's headless shell | Chromium is BSD-3-Clause; the browsers are their makers' | whichever is installed, or the shell fetched into the kit; without either, FETCH reads the page with curl |
| `TOOL:` | an MCP server you add in Settings, Tools | the server's own | wherever you got it -- often `npx`, which fetches it on its first run |
| `PYTHON:` | Crucible's own Python | PSF | see above |
| `SCREENSHOT:` on macOS | `screencapture` and `sips` | Apple's | part of macOS |
| the mouse and keyboard on macOS | `osascript` | Apple's | part of macOS |
| the screen on Windows | PowerShell with the .NET classes Windows ships | Microsoft's | part of Windows |
| the screen on Linux | `xdotool` or `ydotool`; `grim`, `gnome-screenshot`, `spectacle`, `scrot` or ImageMagick's `import` | BSD, MIT, GPL and LGPL variously | your package manager |
| the words in a picture, on macOS | the Vision framework, through `osascript` | Apple's | part of macOS |
| the words in a picture, on Windows | `Windows.Media.Ocr`, through Windows PowerShell | Microsoft's | part of Windows |
| the words in a picture, on Linux | [tesseract](https://github.com/tesseract-ocr/tesseract), or [RapidOCR](https://github.com/RapidAI/RapidOCR) | Apache-2.0 | your package manager, or RapidOCR fetched into the kit |

The kit, fetched only for what the machine does not have:

| piece | what | license | fetched from |
|---|---|---|---|
| GitHub's command line | [gh](https://github.com/cli/cli) | MIT | its GitHub releases |
| Node.js | [Node.js](https://nodejs.org), the current long-term release | MIT, with its bundled parts under theirs | nodejs.org/dist |
| a headless browser | [Chrome for Testing](https://googlechromelabs.github.io/chrome-for-testing/)'s headless shell | BSD-3-Clause (Chromium) with its third-party notices | Google's storage for Chrome for Testing |
| Git, on Windows | [MinGit](https://github.com/git-for-windows/git), Git for Windows' minimal build | GPL-2.0 | Git for Windows' GitHub releases |
| reading pictures, on Linux | [RapidOCR](https://github.com/RapidAI/RapidOCR) and [onnxruntime](https://onnxruntime.ai) in a Python of its own | Apache-2.0; MIT | PyPI, through pip |

Nothing of these is changed, and each keeps its own license file where it was
unpacked. `crucible --uninstall` removes the kit with the rest of Crucible's
data.

## The program a made app runs in

**Make it an app** copies `crucible-app`, which is Crucible's own code
(MIT) built with webview (MIT), beside the page it packages. The page is
whoever made it's -- yours.

## The model Crucible fetches to build with

On a machine with no model at all, the first start fetches one open coding
model, the largest of these that leaves half the machine's memory free:

| model | license | fetched from |
|---|---|---|
| [Qwen3-Coder-30B-A3B-Instruct](https://huggingface.co/Qwen/Qwen3-Coder-30B-A3B-Instruct), Q4_K_M | Apache-2.0 | unsloth's GGUF on Hugging Face |
| [Qwen2.5-Coder Instruct](https://huggingface.co/Qwen) 14B, 7B and 1.5B, Q4_K_M | Apache-2.0 | bartowski's GGUFs on Hugging Face |
| [Qwen3-4B-Instruct-2507](https://huggingface.co/Qwen/Qwen3-4B-Instruct-2507), Q4_K_M | Apache-2.0 | unsloth's GGUF on Hugging Face |

Every one is Apache-2.0, so what is built with it is yours to use however you
like. (Qwen2.5-Coder at 3B is not on the list: its license is research only.)

It lands in your models folder like any model you put there, and is yours to
delete.

The DuckDuckGo search provider reads DuckDuckGo's own HTML results page over
HTTPS; nothing of theirs is carried here, and using it is between you and
them under their terms.

## NVIDIA's CUDA libraries

A CUDA runtime downloaded from **Settings → Runtimes** needs two libraries
from NVIDIA that the driver does not include: the CUDA runtime (`cudart`) and
cuBLAS. Crucible fetches them from NVIDIA's own redistribution site,
`developer.download.nvidia.com`, at the version the runtime was built
against, and puts them beside it. They are NVIDIA's, under NVIDIA's license
for redistributable CUDA components; **Crucible does not carry them**, and a
machine that already has them on its library path uses its own. Removing the
CUDA runtime removes them too.

## Crucible's own Python

Routing, the cook loop and the training environment run on a Python that
Crucible fetches on its first start (or with `crucible --install-python`):
a [CPython](https://www.python.org/) 3.12 build from the
[python-build-standalone](https://github.com/astral-sh/python-build-standalone)
project, at a version and SHA-256 compiled into Crucible, unpacked into its
data folder. CPython is under the
[PSF License](https://docs.python.org/3/license.html); the build carries the
libraries CPython is linked with -- OpenSSL (Apache-2.0), SQLite (public
domain), zlib, libffi, XZ, bzip2, Tcl/Tk and others under their own permissive
licenses, which the build lists in its own `licenses` notes. **Crucible does
not carry it**; it fetches it from the project's GitHub releases. The
orchestrator that runs on it -- `scripts/orchestrator` -- is Crucible's own
code and imports nothing outside Python's standard library.

## Providers

A provider is somebody else's service, reached over HTTPS when you have added
one and pointed an expert at it: Anthropic's Messages API, or any service that
speaks OpenAI's chat-completions one. **Crucible carries none of their code**
-- there is no SDK in the build, only requests written against the two
published wire formats -- and using one is between you and that provider,
under their terms and on your key.

What a provider's tokens cost is read from
[LiteLLM's price list](https://github.com/BerriAI/litellm/blob/main/model_prices_and_context_window.json)
(MIT), fetched at most once a day into Crucible's data folder, and only once a
provider has been added. The request asks for a public file and says nothing
about the machine.

## The training environment

Fine-tuning runs in a Python environment that Crucible builds from its own
Python -- on its first start, or with `crucible --install-trainer`, which the
installers run unless told `--no-trainer`. **None of it is distributed with
Crucible.** pip fetches it from
PyPI and from PyTorch's own index onto your machine, into a folder under
Crucible's data directory, exactly as if you had run pip yourself. Crucible
carries no copy, and a `crucible` binary is unaffected by any of these
licenses.

What it asks for directly:

| package | license | what it does here |
|---|---|---|
| [PyTorch](https://github.com/pytorch/pytorch) | BSD-3-Clause | the tensor library everything below stands on |
| [transformers](https://github.com/huggingface/transformers) | Apache-2.0 | loads the base model and its tokenizer |
| [peft](https://github.com/huggingface/peft) | Apache-2.0 | LoRA and QLoRA adapters |
| [accelerate](https://github.com/huggingface/accelerate) | Apache-2.0 | places the model on the device |
| [datasets](https://github.com/huggingface/datasets) | Apache-2.0 | reads a Huggingface dataset |
| [bitsandbytes](https://github.com/bitsandbytes-foundation/bitsandbytes) | MIT | the four-bit quantization QLoRA is |
| [safetensors](https://github.com/huggingface/safetensors) | Apache-2.0 | the weight format the result is saved in |
| [sentencepiece](https://github.com/google/sentencepiece) | Apache-2.0 | tokenizers that need it |
| [protobuf](https://github.com/protocolbuffers/protobuf) | BSD-3-Clause | a sentencepiece dependency |
| [gguf](https://github.com/ggml-org/llama.cpp/tree/master/gguf-py) | MIT | writes the exported GGUF |
| [numpy](https://github.com/numpy/numpy) | BSD-3-Clause | arrays, everywhere |
| [MLX](https://github.com/ml-explore/mlx) and [mlx-lm](https://github.com/ml-explore/mlx-lm) | MIT | the Apple Silicon path, instead of PyTorch |

Their own dependencies come with them -- around seventy packages in total,
under Apache-2.0, MIT, BSD, MPL-2.0 and the Python Software Foundation
license. `crucible --trainer-status` names the folder; the `*.dist-info`
directories inside it carry each package's own license text.

**The CUDA libraries are proprietary.** A PyTorch built for NVIDIA hardware
pulls in cuBLAS, cuDNN, NCCL and a dozen more as `nvidia-*` wheels, and those
are published by NVIDIA under NVIDIA's own license, not an open-source one.
They are the majority of the environment's size. This is what installing
PyTorch on an NVIDIA machine has always meant, and it is worth being explicit
that Crucible asking pip for `torch` is what puts them there. The CPU and MLX
flavors pull in none of them.

**Models are not covered by any of this.** Crucible ships no model weights and
downloads none of its own. Whatever GGUF files you put in your models
directory carry their own licenses, which are between you and whoever trained
them -- and so does any base model a fine-tune starts from, which the trainer
downloads from Huggingface on your instruction and under whatever terms that
repository sets.
