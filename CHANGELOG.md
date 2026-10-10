# Changelog

Crucible is versioned `MAJOR.MINOR.PATCH`. A release is a `v`-prefixed tag on
GitHub, which builds the installers for macOS, Windows and Linux and attaches
them to the release. Running Crucible checks for a newer tag once a day and says
so in **Settings → About**; updating is the same one-line installer that put it
there, and it keeps your config, models and history.

## 0.8.9 — 2026-10-10

**Build: give it a directive and it makes the software.** A new tab beside
Chat. Type what you want -- "I need a budgeting program that charts my gross
and net revenue" -- and an architect writes a plan of tasks, each task is
given to the expert that fits it, the tasks are worked -- several at once --
with the same tools a chat turn has, the result is checked, and the
architect writes up what was built, how to run it and what is left. The plan is shown before the first task
starts, and *go*, or what to change, is typed into the box; a build can ask
you things on the way, as a cook can, and Stop and finish leaves the project
running.

- **Agents.** The view is the work rather than a log of it. The architect
  and a row per task, with the expert that has it and where it got to, are
  in the side menu under the experts, in sight from every view; click one
  and Build shows its work -- what it was asked, every step it took, the
  files it changed, which open in the code pane. The journal is the same one
  a cook keeps, with the plan and the tasks on top, so History lists builds
  beside cooks and opens them the same way.
- **The experts it needs.** A task is routed the way a HANDOFF is: the
  delegator is shown what it needs and picks the seat. When nobody on the
  roster fits and **Settings, Build** names a model for new agents, the
  build adds a seat for it -- "CSS Layout", "SQL Schema" -- and keeps it,
  with its examples written by the delegator like any seat added by hand.
  That is the cheap half of teaching the experts it needs; for the expensive
  half, every task a provider's model did is recorded, with the files it
  made, in the project's history folder, where Create can take it as data
  to fine-tune a local expert on.
- **Commits as it goes.** After each task that finishes the build commits
  what changed, starting a repository when the project has none, so a bad
  task is a `git revert` rather than an archaeology. Off in Settings, Build.
- **Agents work side by side.** A task starts when the ones it comes after
  are done and nothing running touches its files, up to **Agents at once**.
  A provider's model takes any number of agents together; a model on this
  machine takes one at a time, because two agents on one model only take
  turns at it and throw away each other's cache, and two different local
  models run together when the memory holds both. A build runs on a thread
  of its own, so the chat stays open while it works, and each has its own
  Stop. Underneath: the core and the orchestrator now have any number of
  calls in flight on the pipe between them, each saying whether it is the
  chat's or the build's; a model on this machine is held by a lease while a
  seat uses it, so loading one for the chat can never free the one an agent
  is generating with; and a provider's client takes requests from several
  threads, with Stop cutting only the ones it is for.
- **Frontier models for the heavy lifting, local ones for the rest.** The
  architect marks each task large or small, and with both kinds of seat on
  the roster a large task goes to a provider's model and a small one to a
  model on this machine, each chosen by the delegator from its own kind.
- **Programs for your own computer, made as pages.** Asked for something a
  person uses on their own machine, the architect plans it as HTML, CSS and
  JavaScript, with charts drawn in its own code and nothing from the
  internet, keeping its data through `window.crucible.load()` and `save()`.
  That is the one kind of program every machine runs with nothing
  installed, the one the preview can show as it is built, and the one
  **Make it an app** turns into a program.
- **A change is a sentence.** "Make the button blue", to a project that
  exists, is one task: no plan to agree to, no write-up -- what the agent
  says it did is the account -- and the preview shows the result.
- **What a build learns is kept** for teaching local models: every task a
  provider's model did, with its files; every plan a provider's architect
  wrote, in `teach/architect.jsonl`; and which seat each finished task went
  to, in `teach/delegator.jsonl` -- what a local model would be fine-tuned
  on to plan and to split the work.
- **It knows the machine.** The architect and every agent are told what is
  on this machine's PATH and which shell runs commands, so a plan written on
  a Mac says `python3` rather than discovering, a task later, that there is
  no `python`. And a small model that says DONE to a task it has not started
  is sent back once, when the plan named files and none was written; the
  second DONE is taken at its word and the write-up says what was not made.

**Make it an app.** The preview's page, as a program you open like any
other: everything it uses put inside it, beside a small window program of
Crucible's own -- `crucible-app`, which has no models and no engine in it --
in Applications on a Mac, the Start menu and the desktop on Windows, and the
applications menu on Linux, with no administrator asked. Its data is kept
in your own data folder, so making it again keeps what was typed into it.

**Crucible fetches what it needs.** On the first start, besides its Python
and the runtimes: the programs experts reach for that the machine lacks --
GitHub's command line, Node.js, Chrome's headless shell, MinGit on Windows,
and on Linux a reader for the text in pictures -- into its own folder, put
ahead of the machine's own on Crucible's PATH and nowhere else; and on a
machine with nothing to build with, an open coding model sized to its
memory, seated as Programming. `crucible --install-kit` fetches the same
from a terminal, and Settings, Tools says what is there and fetches what is
missing. The words in a picture are read by what the machine has --
Vision on a Mac, Windows' own OCR engine -- so a local model that reads text
only is given them without tesseract.

**More of what an expert can do.**

- **EDIT** changes part of a file with SEARCH and REPLACE blocks, matched
  line by line when the indentation was copied wrong and refused when the
  lines are in the file twice -- where a small model rewriting a whole file
  to change one color was the likeliest way to break it.
- **FIND** searches the project's files for text or a pattern, `in *.css`
  for some of them, without what the project depends on.
- **RENDER** draws an HTML page or an SVG into a PDF or a PNG with the
  headless browser: how a model makes a document or a picture.
- **TOOL** calls a tool of an MCP server added in Settings, Tools -- GitHub's,
  a database's, any of them -- for every expert, local or not. Its tokens stay
  in the config and are never sent to the page.
- An action after a NOTE in the same reply is the call: a 14B coder that
  wrote "NOTE: starting the task" above every WRITE had every WRITE thrown
  away, eight rounds running. A reply that is only a note is idle now, and
  the agent is told to act.

**Code, Source, Preview.** Three panes beside the agents, for a project
whether or not a build is running.

- **Code** is the project's files: a tree, and a file opened to read -- or
  to edit and save, with Ctrl+S -- or a picture looked at.
- **Source** is version control: the branch and remote, what changed since
  the last commit with the diff of any file, a commit box, Pull and Push, a
  box for any git command, and the history. With `gh` installed, **Publish
  to GitHub** makes a repository for the project and pushes it, and
  **Ship** runs the command that packages it -- the plan's, or yours -- and
  tags a release. With the project on GitHub, the latest **CI** runs are
  listed, passed or not.
- **Preview** shows an HTML page of the project in a frame, with its
  stylesheets, scripts and pictures put inside it by Crucible, or a page by
  address -- the server a build left running. **Pick an element**, and a
  dashboard beside the frame shows what it is and the things most worth
  changing by eye: padding, margin, gap, font size, font, weight, color,
  background, rounding, alignment, width -- each applied to the page as you
  change it, with a plus and a minus to nudge the lengths. **Apply to
  source** makes the whole set in the stylesheet as a build of one task,
  because a change the page forgets on reload is not a change -- and the
  page reloads with it. One pick ends the picking, as a browser's inspector
  does. The page is given `window.crucible.load()` and `save()` before its
  own scripts run, so an app keeps what is typed into it while it is
  previewed. A build that finishes with a page shows it, and an open
  preview shows a page again as an agent rewrites it -- or, in the middle of
  tuning, says it has changed rather than throwing the tuning away.

**The tools: what an expert can do, local or not.** Both the frontier
models and the local ones now get the same set, in the same text protocol
every model can follow, each group behind its own switch.

- In a trusted folder, as before: LIST, READ, WRITE, RUN -- and RUN can
  name its shell, `RUN zsh:`, `RUN powershell:`, `RUN cmd:`, `RUN fish:`.
  READ now reads a PDF, a Word file or a spreadsheet as its text, a web
  page as its source -- tags and all, since an expert reading one is about
  to change it -- and a picture as a picture, to a model that sees one.
- **PYTHON:** followed by a block runs it on Crucible's own Python, in the
  project. **GIT:** runs git in the project, and **GH:** GitHub's command
  line when it is installed: `GIT: log --oneline -5`, `GH: pr list`. An
  argument that would take git somewhere else -- `-C`, `--git-dir` -- is
  refused; the rest is git.
- **START:** runs a command and leaves it running -- a development server
  -- named p1, p2; **LOGS:** reads what it has printed and **STOP:** stops
  it. They outlive the turn and the cook, and are stopped when Crucible
  closes; **Settings, Tools** lists what is running.
- With the web on: **SEARCH:** as before, and **FETCH:** reads a page as its
  text, rendered in a headless Chrome, Chromium or Edge when the machine has
  one -- scripts and all, which a documentation site needs -- and through
  curl when it does not. Search's default provider is **DuckDuckGo** now,
  which needs no key and no server; Wikipedia, searxng and Brave are still
  there. A config that says wikipedia keeps saying it.
- **Using the computer.** Off until it is switched on in Settings, Tools,
  and the one switch there worth reading twice. On, an expert can
  **SCREENSHOT:**, **CLICK:** a pixel, **MOVE:**, **TYPE:**, press a
  **KEY:** -- enter, ctrl+c, cmd+shift+s -- and **SCROLL:**, with what each
  platform already has: `screencapture` and `osascript` on a Mac, `xdotool`
  and a screenshot tool on Linux, PowerShell on Windows. A screenshot goes
  to a model that sees pictures as a picture; with **tesseract** installed,
  a local model that reads text only gets the words off it. The pictures a
  tool makes are in the transcript and the journal as something to open,
  by path, so a journal does not carry a megabyte a step. On a Mac the
  first screenshot and the first click ask for Screen Recording and
  Accessibility; the tool says so when they are refused.

Nothing of any of this is linked into Crucible or distributed with it: git,
gh, the browser, tesseract and the screen tools are programs the machine
has or does not, run the way curl is, and THIRD_PARTY.md names them.

**Cook is folded into Build.** A cook was one goal worked in passes until
it was stopped, and a build is the same work with a plan on top: the same
tools, the same journal, the same asking and handing over. One tab does both
now. A directive about a project that already exists -- "make the failing
tests pass" -- is planned and worked like any other, and a cook from before
opens from History as it always did. `cook.start` stays on the surface for a
caller that wants the plan-less loop.

**Agents in the side menu.** Under the experts, a build's agents: the
architect, and one for each task, saying whose it is and how far it has got,
each opening its work in Build. With the side menu open the Build view gives
the work the whole width; folded, it lists them itself, and the rail keeps a
dot for each agent at work. A seat a build made for itself is on the roster
with the rest -- routed to, edited and ejected like any seat, and marked in
the config file with `"origin": "build"`. A seat at work in a build is lit,
and clicking it opens the task it has; clicking an idle one opens its
settings as before.

**The models at work, in the corner.** The foot of the right-hand panel
shows the local models in memory -- the delegator and each expert, with its
share of the machine's memory -- and under them the frontier model in use:
what it has been asked this session and this month, what that cost at list
prices, how much of each rate limit its provider says is left, and how full
its context is. Every request to a provider is counted, the chat's and each
agent's alike, with cached tokens counted apart because they are billed for
less. Providers do not publish prices where a program can ask, so the list
comes from LiteLLM's, fetched at most once a day and only once a provider
is added; a model not on it shows its tokens and no cost rather than a guess.

**The Scratchpad is out of sight.** A chat with no project keeps its folder
in `~/.crucible/Scratchpad` now, hidden -- by its name on a Mac and Linux,
by the folder's attribute on Windows -- rather than in a `~/Crucible` in every
listing of the home folder. The first start moves the old one, and every
chat, build journal, recent project and trusted folder that named a path in
it follows; a folder an older Crucible made again while it was still
running is merged back rather than duplicated.

**Code is colored everywhere it is shown, HTML included.** HTML, XML, SVG
and Vue are colored by tag, attribute and value, with what a `<style>` and a
`<script>` hold colored as CSS and JavaScript; CSS by selector, property and
value; Markdown by heading, list, code and link, a fenced block in its own
language; and some thirty more, from PowerShell, batch files, Dockerfiles and
Makefiles to C#, Swift, Kotlin, Elixir, Haskell, Terraform and TeX. A
Dockerfile or a Makefile is known by its name. And the code pane's editor
colors what is typed as it is typed: the text is edited in a textarea with
clear letters over the same text colored, so undo and selection are still
the textarea's own.

**A local model that sees.** A GGUF with its `mmproj` projector beside it --
the way vision models are published -- is given pictures as pictures, read
by llama.cpp's multimodal library, instead of the words in them.

**An edit a small model gets nearly right is taken.** EDIT now takes off
READ's line numbers when a model copies them with the lines, and matches with
blank lines set aside; when nothing matches, the nearest lines in the file are
said back, numbered, so the next try copies the file's own text rather than
the model's memory of it. A 14B coder that had spent eleven rounds on "those
lines are not in the file" was the reason. A refused edit keeps the blocks
the model wrote, so the step shows what it tried beside why it did not apply
-- one turned out to be Chart.js's documentation example, recited -- and when
nothing like them is in a short file, the model is told to write it whole. A
block that goes straight from SEARCH to REPLACE takes its lines out, which is
how a small model asks for a deletion.

**A build does what it is told about its size, and checks only what can be
run.** Asked for "in one task", it is one task, whatever the architect cut it
into: the pieces become that task's steps. A "check" the architect wrote as a
description -- "open index.html in a web browser and verify the charts" -- is
not run as a command, where it failed and cost a task spent making a sentence
pass; a page app has no check, since the page is shown rather than run.

**Crucible's icon, everywhere.** On Windows the icon is inside `crucible.exe`,
so the taskbar, Explorer and the window itself show it rather than Windows'
blank one. On Linux the window says it is `crucible`, which is how the
desktop matches it to its menu entry, and names its themed icon for the
window managers that draw a window's own. `packaging/macos/dmg.sh` stops at
the bundle when asked for a `.app`, so a bundle made to look at is the one a
release ships, icon and signature included.

**Fewer dots.** The parts of a line are set apart by space now rather than by
a dot between them.

**A Mac is not asked to compile Vulkan.** The first start fetched Metal and
then reported Vulkan as a failure beside it, on every start, because Vulkan
on a Mac is MoltenVK with a Homebrew toolchain and no prebuilt module. Metal
is the GPU on a Mac; Vulkan stays on the Runtimes page for anyone who wants
it, and the red card is gone.

**Smaller things.**
- Ctrl+2 is Build; Create and History are Ctrl+3 and 4.
- Running the test suite opened a Crucible window each time: the test that
  asks every method of a surface with nothing behind it asked `self.restart`,
  which started the built program. A restart now needs a window to close.
- On a Mac, `tests/test_install.sh` stopped after its first few checks and
  reported success: bash 3.2 treats an empty array as unset. It runs all of
  them now, and six that had gone stale were brought up to date.
- A failed C++ test on GitHub Actions is an annotation on the commit, naming
  the case, the file and the line, which anyone can read without signing in.
- A build left mid-way -- Crucible closed while it ran -- says interrupted in
  History, rather than asking or working for ever after.
- The preview closes a script whose closing tag is written in capitals.
- `RUN: RENDER: index.html to page.png` -- one of the verbs handed to the
  shell, where it was a command not found -- is taken as the verb.
- The settings have a **Build** page: who plans, the model for new agents,
  whether to show the plan first, commits, and rounds per task.
- The orchestrator protocol is 2: a journal step says which task of a build
  it belongs to and where any picture it made is, and a journal from before
  reads as a cook with neither.

## 0.8.6 — 2026-10-06

A hotfix for three things found in 0.8.5.

**Scrolling up while a reply streams.** The chat followed the bottom for anyone
within 80 pixels of it, measured at every redraw -- and a reply redraws up to
thirty times a second, as does the delegator's loading ring after one. A mouse
wheel on Windows, or a trackpad on a Mac, moves a few pixels at a time, so the
reader was always "near the bottom" and was pulled back before getting
anywhere. Whether to follow is now decided by what the reader does: turning the
wheel up, or scrolling up any other way, lets go at once; coming back to the
bottom picks it up again. The test window in Create had the same fault, worse:
it held its transcript at the bottom on every redraw.

**Delete chats, and take projects off the list.** A bin shows on a recent
chat or project when the pointer is over it, and asks first. A chat is deleted
for good, and a chat with a scratch folder of its own takes the folder with it.
A project is only taken off the list: its folder is not touched, and opening it
again puts it back.

**Less text.** Settings, Create and the dialogs said a paragraph where a few
words do, and now say the few words or nothing. Messages that point at a
setting call it by the name it has on screen -- "Longest reply", not "Max
tokens", which no screen said. On **General** the models folder comes first. **Generation** no longer has *Show a reasoning model's
working*: each reply's thinking starts shut, and opening one keeps that one
open. A reply's thinking is saved with the conversation now, so a chat opened
again has it to open; it is still never sent back to the model.

## 0.8.5 — 2026-10-05

**Crucible fetches everything it needs.** A download carries the program and
nothing else, and for a long time the rest was a visit to the settings screen.
Now, on its first start, Crucible fetches what is missing in the order it is
needed, with the progress above the empty chat: its own Python, the compute
runtimes this machine can use (the CPU always, CUDA with NVIDIA's libraries
where there is an NVIDIA driver, Metal or Vulkan), and the training
environment. A chat works as soon as Python and a runtime are in; anything that
fails says why and has **Try again**. `crucible --install-python` does the
first part from a terminal, and the installers run it. Models are not among
what it fetches: those are always yours to bring.

**Its own Python.** A pinned CPython 3.12 build from the python-build-standalone
project, checked against the SHA-256 compiled into Crucible and unpacked into
its data folder. The training environment is built from it, so fine-tuning no
longer needs a Python on the machine -- a Mac or Windows PC usually has none --
and the installers no longer ask a Linux system for one.

**Routing and cooks run in Python.** The two pieces of orchestration --
deciding which expert answers, and the cook loop -- are a Python package now,
`scripts/orchestrator`, run in a process of its own on Crucible's Python and
spoken to over a pipe. The core keeps what must not be duplicated: the models,
the cards, the trusted folder and its tools, the journal. The router was moved
without changing a decision: on the 54-prompt benchmark it picks the same
expert as before for every prompt, with the same confidence, and
`crucible-routebench` now measures the Python router the app actually runs.

**Routing that is not fooled by the order of the list.** A delegator leans
towards a seat for where it sits on the roster as well as for what it is --
gpt-oss-20b, shown five experts, sent biology questions to Chemistry because
it was fourth. The roster is now shown three ways -- as listed, reversed, and
turned halfway -- and the three answers averaged, and the delegator is asked
for the subject's name, which is what it is scored on. On a five-expert roster
of fifty questions: LFM2.5-1.2B from 45 to 49 right, gpt-oss-20b from 39 to
48; on the nine-subject benchmark, gpt-oss-20b from 50 to 54 of 54 and LFM
unchanged at 50. With gpt-oss as the delegator a pH question goes to Chemistry
now; LFM still finds it as much Mathematics as Chemistry, and below the
confidence floor, so with a default expert set it goes there. The price is a
routing pass three times over: a few hundred milliseconds with a small
delegator, a second or two with gpt-oss.

**Naming a chat and writing an expert's examples are Python's too**, like the
routing they serve. A reasoning delegator's thinking no longer ends up as a
chat's name -- with gpt-oss as the delegator, chats were named after their
first words because its answer came after its working.

**Nothing is loaded until it is needed.** The delegator used to load the moment
Crucible opened. It now loads for the first prompt, and is brought back after
each one for the next; opening the window costs a card nothing. Changing the
delegator, or a runtime arriving, no longer loads one either.

**Hardware settings take effect.** A loaded model keeps the cards it was loaded
onto, so a change in Settings → Hardware -- the mode, the order, one card,
VRAM only -- or to a seat's context or GPU layers did nothing until Eject or a
restart. Whatever a change affects is now let go, and the next prompt loads it
the new way.

**MLX folders, however they are kept.** A folder of MLX models is found four
levels down -- in a folder of them, a publisher's folder inside that, or
Hugging Face's own cache -- and a models folder that is itself one MLX model is
listed too. On a Mac the training environment Crucible now fetches on its first
start is also what runs them, so they are offered rather than marked "cannot
run here".

**Windows: no more closing when a prompt is answered.** A command's output on
Windows is in the console's code page, not UTF-8 -- and so is a Latin-1 file
anywhere -- and saving a conversation that held some threw from the window's
own thread, which ended the program. Command output is now read in the
console's code page, everything kept is written as UTF-8 whatever it held, and
nothing the window does after an answer can end it. And should Crucible ever
crash again, it writes where to `crash.log` in its data folder and says so the
next time it starts.

**Windows: System32 is not a project.** Older versions opened whatever folder
they were started in -- System32 from the Start menu, the install folder from a
shortcut -- and remembered it. A drive's root, the home folder itself, the
system's folders and Crucible's own are never listed as projects or recent
chats again, and never remembered.

**A delegator too big for its card finds another.** The delegator was given one
card of its own -- the last in the priority order -- on the assumption that it
is small. A 20B delegator did not fit on a 12 GB card while a 16 GB one beside
it had room, and routing fell back to keywords with a long warning in the chat.
It now goes to whichever card holds it, and is divided across them when none
does.

**The chat is the conversation and nothing else.** What Crucible has to say --
a setting applied, a runtime ready, a model that would not load -- goes to the
status line at the top of the side menu, whose hover lists the last few. A
delegator that could not be loaded is marked on its own row, with the reason.

**Smaller things.**
- A code block a model left unlabeled is colored in the language its code is
  written in, and a script's first line written above the fence -- gpt-oss
  writes `#!/usr/bin/env python3` there -- is put back inside it.
- A cook that keeps going in circles is stopped, as it was always meant to be:
  restarting it reset the count that would have, so it never was.
- A cook's thought is cut at a word in the journal, not through one.
- "no expert has no model" now reads "no expert was named".
- `crucible-routebench --cases FILE` measures a roster of your own, and
  `crucible-smoke` sends one prompt through the whole engine with no window --
  which is how a Windows build is run here under Wine.

## 0.8.1 — 2026-10-05

**The macOS download opens.** The disk image's application was not signed at
all, so on Apple Silicon a downloaded copy was "damaged" to Gatekeeper -- and
damaged has no Open Anyway. It is signed ad hoc now, every library and then the
bundle, and checked the way Gatekeeper checks it before the image is made. It
is still not signed with an Apple certificate, so the first open asks: System
Settings → Privacy & Security → Open Anyway, as the image's read-me says. The
application has an icon of its own on the Mac, drawn to Apple's grid so it sits
at the same size as its neighbors in the dock.

**MLX models.** A folder holding an MLX model -- a `config.json` and its
weights as `.safetensors`, the way LM Studio and the mlx-community uploads lay
them out -- is listed in every model menu and runs on Apple Silicon through
MLX's own server, started from the training environment's Python on this
machine. The models folder is read two levels deep for GGUF files as well, so
LM Studio's `publisher/model/` folders need no moving.

**Choose who answers, and how hard it thinks, from the box.** Two menus beside
the **+**: *Delegator* -- which picks an expert for each prompt -- or one
expert, to send every prompt to it until changed back; and reasoning effort,
moved here from Settings → Generation. A model with no effort setting is not
sent one: a local model gets the line only when its template reads it.

**A recent-chats panel, and chat with no project open.** A panel on the right,
folded and widened like the side menu, lists recent conversations across every
project, each named for what it is about -- the delegator reads the first
exchange and writes two to four words, "Math homework" -- and below them the
projects you have opened. Choosing a conversation opens its folder and carries
on with the expert's memory of it; choosing a project opens it. **New chat**
starts a conversation that belongs to no project: its first message makes it a
scratch folder of its own, `~/Crucible/Scratchpad/<when>`, for anything it
makes. Opening and switching projects happens here now, and the project button
is gone from the top bar, which keeps only the path of the folder the chat works
in. Clicking the path opens that folder in the system's file browser -- Files,
Nemo or Dolphin, Finder, Explorer.

**The window remembers its shape.** The side menus' widths, the box's height and
the expert prompts go to were kept in the webview's own storage, which did not
outlive the window. They are kept in a file of Crucible's own now, and come
back when it starts.

**An empty code block before an answer is gone.** A model given tools to look
at a project looks -- and a new chat's folder has nothing in it, so the listing
came back blank and was drawn as an empty box. A command that prints nothing
is not drawn; an expert is told when the folder is empty; and a chat turn does
not run the same command twice.

**Smaller things.**
- Send is an arrow in an orange box; Stop, while something runs, the same box
  in gray. Auto says whether it is on, *Auto on* or *Auto off*, at one width.
- A reopened conversation shows what it cost, rather than carrying over the
  count of whatever was open before it.
- The chat no longer opens with a line per device and the GPU split, which
  Settings, Hardware already shows; opening a project or a chat's scratch
  folder is not announced in it either, since the top bar shows the path;
  nothing is written under *Ask anything* and its suggestions; and Send, Auto
  and the effort menu have no hover text.
- Settings → Runtimes could call every runtime idle, driving nothing, though a
  model was running on one: it showed what the window was told at startup,
  before the engine had loaded any. It asks again each time Settings opens.
- The right-hand panel's titles and buttons stay on one line however narrow it
  is dragged; a title is cut short before a button wraps.
- Cook's empty screen and the README say what a cook does: works in passes,
  each improving on the last, until you stop it. They said "until it is done",
  and the README still asked for a time limit the screen stopped taking in
  0.7.1.

## 0.8.0 — 2026-10-05

**Attach files, photos and folders.** A gray **+** at the bottom left of the
box -- in Chat and in Cook -- opens a menu: *Add files or photos* (Ctrl+U) and
*Add folder*, each through the platform's own file dialog. What you choose sits
in the box as tiles above the text: a picture as itself, a PDF as its first
page, anything else as its name and type, each with an × to take it out. Keep
typing and send them together. PDFs, `.docx` and old `.doc`, `.xlsx` and
`.xls`, `.pptx` and `.ppt`, OpenDocument, RTF, EPUB, HTML and every kind of
plain text are read into the prompt, above what you typed and cut to what fits
the expert that answers, with the cut said out loud. A folder sends its
readable files and skips `node_modules`, build output and `.git`. Pictures go
to a provider's model as pictures, shrunk to 1568 pixels; a model on this
machine is told one was attached that it cannot see, and a provider that
refuses pictures gets the text and a line saying the picture was left out.
Everything is read here -- miniz for the zip-based formats, Crucible's own
readers for the rest, and `pdftotext` or `catdoc` when the machine has them.
A conversation and a cook's journal keep what was attached, and Ask again
sends it again.

**Or drag them onto the window.** Anything dragged over Crucible blurs the
window behind an outline in the flame that says *Drop files or folders here*,
and what you let go of goes in the box -- Chat's or Cook's, whichever is
showing, and Chat's from any other view. When the box is shut the outline
turns gray and says why. On Linux the files are attached where they are; on
Windows and macOS, whose webviews never tell a page where a dropped file
lives, Crucible keeps a copy in its own folder for two weeks and attaches
that.

**A downloaded CUDA runtime starts.** The prebuilt CUDA backend needs NVIDIA's
CUDA runtime and cuBLAS libraries, which the driver does not include, so on a
machine without the toolkit it installed and then could not load. Installing
it now fetches those two from NVIDIA's own download site and puts them beside
it; Settings says when a CUDA runtime is installed but missing them, and
Reinstall fetches them. Installing no longer asks for `nvcc` before it has
tried the download that does not need it.

**Loading stuck at 21% is fixed.** With **Dedicated VRAM only** on, models
were read with direct I/O, and on the Vulkan backend llama.cpp's upload from a
direct read waits on an event that never fires: the load stopped at whatever
percentage it had reached and nothing, Stop included, could end it. On Vulkan
the model is now memory-mapped instead; the setting still refuses a model that
will not fit, which is the part of it that matters.

**A cook acts with gpt-oss.** That model calls tools in a format of its own --
a message addressed to `container.exec` or `functions.write` -- and sometimes
writes `LIST: .` where a channel name goes. Every one of those used to read as
an empty reply, and a cook would answer "(said nothing)" hundreds of times.
They are translated into the commands they mean now, a round with nothing in
it no longer goes into the model's own history, and a cook that takes no
action eight rounds running stops and says why.

**Stopping does not lock anything up.** Stop now on a cook that was asking you
a question left the engine waiting for an answer that would never come, and
every later request queued behind it. It wakes now, as it already did for an
edit waiting on you. Stop also interrupts a prompt partway through being read,
and a handoff that could not load its new expert no longer carries on with the
old one's freed memory.

**The delegator is back before you need it.** It is loaded when Crucible
starts and again the moment a prompt or a cook ends, however it ended, while
the box is already free for the next prompt. "Load on demand" decides only
when it is freed -- the instant it has routed -- not when it returns.

**Priority order fits what even mode fits.** The check before a load measured
each card's share as an average layer, and priority mode fills the first cards
with real layer sizes right to the top -- so it refused models that loaded in
even mode on the same cards. It counts the actual layers now.

**One models folder.** Every model dropdown -- the delegator's, each expert's,
New expert's -- lists the models folder and the providers' models, and a
fine-tune made in Create is written into that folder when it finishes. The
per-seat Browse buttons are gone; **Change folder** is beside the list.

**Smaller things.**
- The loading ring is a circle again, with the percentage in its middle. It
  was rotated about the wrong point and swung half off itself, over the name.
- Adding a provider asks for an address, a key and a model, with a template
  list to fill the address in. Which API it speaks is worked out from the
  address.
- **Auto** is on the Cook screen too, and a cook asks before it writes when it
  is off. The duplicate checkbox in Settings → Tools is gone.
- Settings → Hardware no longer asks which card holds the output in priority
  mode.
- Create is titled Create, with New expert under its one line of explanation.
- The macOS build compiles again.
- An edit you allow stays in the transcript the way it was offered: a new
  file as colored, numbered code, a change as the lines that moved, matched
  up so a line it left alone reads as left alone. It used to turn into a gray
  unified diff the moment it was written, and its count agreed with neither.
- What an expert says before reading or writing a file and what it says after
  are two paragraphs, and an answer that took several actions keeps what it
  said before each one. They used to run together mid-sentence, or replace
  each other.
- The line beside the expert's name no longer says "router model" on every
  turn; it names how a turn was routed only when it was unusual -- pinned, a
  fallback, keywords.
- On Windows, a path with an accented letter in it -- a project, the models
  folder, an attachment -- is found. Windows read Crucible's UTF-8 paths in
  its old ANSI code page; the program now declares UTF-8 as its own.

## 0.7.5 — 2026-10-04

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
