# SPDX-License-Identifier: MIT
"""The build loop: a directive, a plan, and an agent for each piece of it.

A cook is one goal worked in passes until it is stopped. A build is a
directive -- "a command-line todo app in Python, with tests" -- turned into
software: an architect writes a plan of tasks, each task is given to whichever
expert fits it best, the tasks are worked one after another, the result is
checked, and the architect writes up what was built and what is left.

Four things are worth knowing before reading it.

The plan is JSON the architect writes and this side reads. A model asked for
JSON writes it inside a fence, with a sentence before it, with a trailing
comma -- so the reader is forgiving, validates what it gets, and asks once
more with the reason when it cannot use it. A build that cannot get a plan
at all becomes one task, which is the directive itself.

Each task is routed the way a HANDOFF is: the delegator is shown what the
task needs and picks the seat. When nobody on the roster fits and a model
for new agents is configured, the build makes a seat for it -- "CSS layout",
"SQL schema" -- which stays on the roster afterwards. That is the cheap half
of "train the experts it needs": the expensive half, fine-tuning one on what
the build's experts did, starts from the records the build keeps.

Tasks run side by side when they can. A task waits for the tasks the plan
says it comes after, and for any running task that touches one of its files;
past that, as many agents work at once as Settings, Build allows. A provider's
model answers any number of them at a time. A model on this machine has one
agent at a time -- two agents on one model take turns at it and throw away
each other's cache, which is slower than one after the other -- so a task
whose expert is a model another agent is using waits for it, and two
different models on this machine run together when the memory holds both.
Each agent's calls carry its seat, so the core answers each of its own.
What the window shows is which agents have work and what each has done; the
journal keeps the whole of it.

Everything is journalled as it happens, like a cook: the plan, every task's
state, every step with the task it belongs to.
"""

import json
import queue
import re
import threading
import time

from . import cook, routing

# At most this many tasks from one plan. A plan of forty tasks is a plan the
# architect could not hold in its head either; the directive is cut up again
# on the next build if the first did not get there.
MAX_TASKS = 16

# How many times the architect is asked again for a plan it did not write
# usably, and how many times the person may send a plan back for changes
# before the build starts anyway.
PLAN_RETRIES = 1
PLAN_REVISIONS = 3

# Rounds a task may take before it is left incomplete and the build moves on.
# Forty is an hour of a small model's work, or ten minutes of a provider's.
ROUNDS_PER_TASK = 40

# The task that fixes a failed check gets fewer: it has one thing to do.
FIX_ROUNDS = 24

# Rounds in a row without an action before a task is given up on, and how
# many of the last few actions may repeat with nothing changing. The cook
# loop's numbers, for the same reasons.
IDLE_LIMIT = cook.IDLE_LIMIT
CYCLE_WINDOW = cook.CYCLE_WINDOW
CYCLE_DISTINCT = cook.CYCLE_DISTINCT
RECENT_TURNS = cook.RECENT_TURNS

# Files worth reading before planning, when the project has them: they say
# what the project is in a way a listing does not.
MANIFESTS = ("README.md", "README", "package.json", "pyproject.toml", "Cargo.toml",
             "CMakeLists.txt", "go.mod", "Makefile", "requirements.txt")
MANIFEST_CHARS = 2500

# How many agents work at once when the settings do not say. One is what a
# build was before agents could work side by side, and what a script that
# drives the orchestrator by hand gets unless it asks for more.
AGENTS = 1
MAX_AGENTS = 8

# Task states that are over, one way or another.
FINISHED = ("done", "incomplete", "failed", "skipped", "stopped")

# What the architect and the agents are told about the kind of program a person
# most often asks Crucible for: one they use on their own computer. A page of
# plain HTML, CSS and JavaScript is the one thing that every machine can run
# with nothing installed, that Crucible can show as it is built, and that it
# can package as a program -- see the app runner, crucible-app. The data
# calls are Crucible's own, because a page shown in the preview has no
# storage of its own and a packaged program needs its data kept somewhere a
# reinstall does not wipe.
APP_GUIDE = (
    "If what is asked for is a program a person uses on their own computer -- a budget, a "
    "tracker, a calculator, anything with buttons, forms and charts -- make it a desktop web "
    "app: index.html, style.css and app.js at the top of the project folder, in plain HTML, CSS "
    "and JavaScript, with no build step, no packages and nothing fetched from the internet. "
    "Draw charts in your own code with <canvas> or inline SVG. Keep the person's data with "
    "`await window.crucible.load()` -- the saved object, or null the first time -- and `await "
    "window.crucible.save(data)` after every change; Crucible provides both, in its preview and "
    "in the finished program. Make it look finished: a clear layout, comfortable spacing, "
    "readable type. Crucible shows the page as it is built and packages it as a program the "
    "person opens like any other, so the plan needs no packaging task, and its \"ship\" and its "
    "\"check\" are \"\": the page is shown, not run.")

APP_NOTE = ("If this is a desktop web app (index.html, style.css, app.js): plain HTML, CSS and "
            "JavaScript, no packages, and nothing from the internet -- no <script> or <link> with "
            "an http address, no CDN, no Chart.js: draw charts yourself with <canvas> or SVG. Keep "
            "the person's data with `await window.crucible.load()` and `await "
            "window.crucible.save(data)` -- Crucible provides both, so use them without asking. "
            "To see the page, RENDER: index.html to a PNG and look at it; it needs no server.")

# What an agent is told after a NOTE with nothing else: a note changes nothing,
# and a model that writes one round after round is not working.
NOTE_PUSH = ("Noted. A NOTE does nothing on its own -- take the next action now: READ a "
             "file, WRITE or EDIT one, or RUN a command, in the same reply as any note.")

# What a person types to start the plan as it is.
GO_WORDS = ("", "go", "yes", "y", "ok", "okay", "start", "build", "run", "do it", "proceed",
            "looks good", "fine", "sure", "yep")

# A person asking for the work in one piece: "in one task", "as a single task".
ONE_TASK = re.compile(r"\b(?:one|a single|1)\s+task\b", re.I)

# What gives away a "command" that is a description of something for a person
# to do -- "open index.html in a web browser and verify the charts".
PROSE = re.compile(r"\b(?:verify|ensure|make sure|check that|confirm|manually|visually|by hand|"
                   r"in (?:a|the) (?:web )?browser|should)\b", re.I)

PLAN_SHAPE = ('{"summary": "<one paragraph: what will be built and how>",\n'
              ' "run": "<the command that runs it, or \\"\\">",\n'
              ' "check": "<one shell command that proves it works -- tests or a smoke run -- or \\"\\"; '
              'never a description of something for a person to do>",\n'
              ' "ship": "<the command that packages it for people, or \\"\\">",\n'
              ' "tasks": [\n'
              '   {"title": "<a few words>", "detail": "<exactly what to make, and how to tell it is done>",\n'
              '    "files": ["<paths it will create or change>"], "needs": "<the expertise, two to four words>",\n'
              '    "size": "<large or small>", "after": [<indices of tasks that must finish first>]}\n'
              ' ]}')


# --- the plan -------------------------------------------------------------------


def one_task(plan, directive):
    """`plan` as the one task a person asked for: the pieces the architect cut
    it into, kept in order as that task's steps, and every file they named.

    Asked for in so many words, one task is what is meant. A small local
    architect plans eight for a change to one file whatever it is told, and
    the person should not have to argue it down.
    """
    tasks = plan["tasks"]
    files = []
    for task in tasks:
        for name in task.get("files", []):
            if name not in files:
                files.append(name)
    steps = "\n".join("- " + task["title"] + (": " + task["detail"] if task.get("detail") else "")
                      for task in tasks)
    merged = {"title": cook.clip(directive, 60), "detail": directive + ("\n\nSteps:\n" + steps if steps else ""),
              "files": files, "needs": tasks[0].get("needs", "") if tasks else "",
              "size": "large" if any(t.get("size") == "large" for t in tasks) else "small", "after": []}
    return dict(plan, tasks=[merged])


def command_or_nothing(text):
    """`text` when it is a command, or "" when it describes one.

    Asked for the command that proves the work, an architect sometimes writes
    what a person would do instead. Run, a sentence is an error, the error is
    a failed check, and a task is spent making a sentence pass.
    """
    text = str(text or "").strip()
    return "" if len(text.split()) >= 6 and PROSE.search(text) else text


def find_json(text):
    """The first JSON object in `text`, or None.

    Fences are stripped and the object is taken from its first brace to its
    last; a trailing comma before a closing bracket, which models add, is
    removed. Anything else that fails to parse is None rather than a guess.
    """
    text = text or ""
    text = re.sub(r"```[a-zA-Z]*", "", text).replace("```", "")
    start = text.find("{")
    end = text.rfind("}")
    if start < 0 or end <= start:
        return None
    candidate = text[start:end + 1]
    for attempt in (candidate, re.sub(r",\s*([}\]])", r"\1", candidate)):
        try:
            parsed = json.loads(attempt)
        except ValueError:
            continue
        if isinstance(parsed, dict):
            return parsed
    return None


def read_plan(text):
    """A plan out of what the architect said: the plan, or the reason it is not one."""
    parsed = find_json(text)
    if parsed is None:
        return None, "the reply held no JSON object"
    tasks = parsed.get("tasks")
    if not isinstance(tasks, list) or not tasks:
        return None, "the plan has no tasks"
    plan = {
        "summary": str(parsed.get("summary") or "").strip(),
        "run": command_or_nothing(parsed.get("run")),
        "check": command_or_nothing(parsed.get("check")),
        "ship": command_or_nothing(parsed.get("ship")),
        "tasks": [],
    }
    for entry in tasks[:MAX_TASKS]:
        if not isinstance(entry, dict):
            continue
        title = str(entry.get("title") or "").strip()
        detail = str(entry.get("detail") or "").strip()
        if not title and not detail:
            continue
        files = entry.get("files") or []
        after = entry.get("after") or []
        size = str(entry.get("size") or "").strip().lower()
        plan["tasks"].append({
            "title": title or cook.clip(detail, 60),
            "detail": detail or title,
            "files": [str(f) for f in files if isinstance(f, (str, int, float))][:12],
            "needs": str(entry.get("needs") or "").strip(),
            "size": size if size in ("large", "small") else "",
            "after": [int(a) for a in after if isinstance(a, (int, float)) and not isinstance(a, bool)],
        })
    if not plan["tasks"]:
        return None, "no task in the plan had a title or a detail"
    if len(tasks) > MAX_TASKS:
        plan["summary"] += "\n(The plan had %d tasks; the first %d are kept.)" % (len(tasks), MAX_TASKS)
    # A dependency on itself, on a task that is not there, or on a later task
    # that depends back on this one is dropped rather than obeyed: the order
    # below has to be an order.
    count = len(plan["tasks"])
    for index, task in enumerate(plan["tasks"]):
        task["after"] = sorted({a for a in task["after"] if 0 <= a < count and a != index})
    for index, task in enumerate(plan["tasks"]):
        task["after"] = [a for a in task["after"] if index not in reaches(plan["tasks"], a)]
    return plan, ""


def reaches(tasks, start):
    """Every task index reachable from `start` along `after` edges."""
    seen = set()
    stack = [start]
    while stack:
        here = stack.pop()
        for after in tasks[here]["after"]:
            if after not in seen:
                seen.add(after)
                stack.append(after)
    return seen


def order_tasks(tasks):
    """The indices in an order that runs a task only after the ones it waits on.

    Stable: among tasks that are ready, the one listed first goes first, so a
    plan written in a sensible order runs in it.
    """
    done, order = set(), []
    while len(order) < len(tasks):
        ready = [i for i in range(len(tasks))
                 if i not in done and all(a in done for a in tasks[i]["after"])]
        if not ready:
            # A cycle read_plan did not break. Take the first task left.
            ready = [i for i in range(len(tasks)) if i not in done]
        order.append(ready[0])
        done.add(ready[0])
    return order


def machine_line(facts):
    """What this machine has, in one line for a prompt -- so the plan says
    python3 where there is no python, and nobody plans around npm on a
    machine without node."""
    if not facts:
        return ""
    programs = facts.get("programs") or []
    missing = [p for p in ("python", "python3", "node", "git") if p not in programs]
    line = ("This machine runs " + str(facts.get("os", "")) + "; commands run in "
            + str(facts.get("shell", "a shell")) + ". On PATH: " + (", ".join(programs) or "nothing of note") + ".")
    if "python" in missing and "python3" in programs:
        line += " There is no `python`: use `python3`."
    if "node" in missing:
        line += " There is no node."
    return line


def plan_prompt(directive, root, survey, feedback="", machine=""):
    text = ("You are the architect of a software build. The directive is:\n\n" + directive
            + "\n\nThe project folder is " + root + ". " + survey
            + ("\n\n" + machine if machine else "")
            + "\n\nWrite a plan as JSON and nothing else, in exactly this shape:\n\n" + PLAN_SHAPE
            + "\n\nRules. When the project already exists and the directive is a change to it "
              "-- a color, a size, a label, a button moved, a field added -- the plan is ONE "
              "task that makes exactly that change, with \"files\" naming the file it is in; "
              "never a rewrite for a small change. For something new, three to twelve tasks, "
              "each one piece of work one person could finish in an hour, in the order they "
              "should happen. The task that makes the project run at all comes first; one "
              "that writes the README or the documentation comes last. Tasks that do not "
              "depend on each other leave \"after\" empty, so they can be worked at the same "
              "time. \"detail\" says exactly what to make and how to tell it is done. "
              "\"needs\" names the kind of expertise in two to four words -- \"Python back "
              "end\", \"CSS layout\", \"SQL schema\", \"technical writing\". \"after\" lists "
              "the 0-based indices of the tasks that must finish first, and is usually empty "
              "or the previous task. \"size\" is large for the heart of the program or a hard "
              "problem, and small for a change to one file, a test, or the documentation. "
              "\"check\" is one command, run from the project folder, "
              "that proves the whole thing works, or \"\" when there is none to run. Prefer "
              "tools the project already uses.\n\n" + APP_GUIDE)
    if feedback:
        text += "\n\nThe person asked for this change to the plan:\n\n" + feedback
    return text


def task_system_prompt(base, directive, plan, task, index, count, root, account, tools, machine=""):
    files = ", ".join(task.get("files") or []) or "whatever it needs"
    return (base
            + "\n\nYou are one agent in a software build, working on a real project on disk. "
              "The directive for the whole build is:\n\n" + directive
            + "\n\nThe architect's plan: " + (plan.get("summary") or "(none written)")
            + "\n\nYour task, " + str(index + 1) + " of " + str(count) + ": " + task["title"]
            + "\n" + task["detail"]
            + "\nFiles it concerns: " + files
            + "\nThe project is at " + root
            + ("\n" + machine if machine else "")
            + ("\n\n" + account if account else "")
            + "\n\nWork in small steps. Read a file before changing it, change part of a file "
              "with EDIT rather than writing it all again, run things to find out whether they "
              "work rather than assuming, and do only this task -- the others have their own "
              "agents, some of them working at the same time as you. When it is finished, say "
              "DONE: and what you made, with anything the next agent must know.\n\n" + APP_NOTE
            + tools)


# --- the build --------------------------------------------------------------------


class Build:
    def __init__(self, core, params):
        self.core = core
        self.params = params
        self.directive = params.get("directive") or params.get("goal") or ""
        self.root = params.get("root", "")
        self.roster = routing.Roster(params.get("roster"))
        self.settings = params.get("settings") or {}
        self.rounds = int(self.settings.get("rounds_per_task") or ROUNDS_PER_TASK)
        self.agents = max(1, min(MAX_AGENTS, int(self.settings.get("agents") or AGENTS)))
        self.journal = {
            "id": params.get("id", ""),
            "kind": "build",
            "goal": self.directive,
            "attachments": params.get("attachments") or [],
            "state": "working",
            "budget_seconds": 0,
            "started_unix": int(time.time()),
            "ended_unix": 0,
            "iterations": 0,
            "outcome": "",
            "question": "",
            "plan": {"summary": "", "run": "", "check": "", "ship": ""},
            "tasks": [],
            "steps": [],
        }
        # The architect's seat, for planning, the finishing pass and the
        # write-up. An agent's seat is its own, in run_task.
        self.seat = cook.Seat()
        self.listing = ""
        self.git_ready = None   # unknown until the first commit is wanted
        self.made = []          # seats this build added to the roster
        self.machine = ""       # what this machine has, for the prompts
        self.decisions = {}     # task index -> who the delegator chose
        self.was_empty = True   # whether the project had nothing in it before the build
        self.planned_from = None   # the prompt the plan answered, and the answer
        # The journal and everything else agents share, under one lock;
        # questions to the person and commits, one at a time each.
        self.lock = threading.RLock()
        self.ask_lock = threading.Lock()
        self.commit_lock = threading.Lock()

    # --- the journal -------------------------------------------------------------

    def publish(self):
        with self.lock:
            self.core.call("cook.publish", {"cook": self.journal})

    def note(self, kind, summary, task=-1, ok=True, detail="", ms=0, changed=None, expert=None,
             picture=""):
        with self.lock:
            self.journal["steps"].append({
                "iteration": self.journal["iterations"],
                "task": task,
                "expert": self.seat.id if expert is None else expert,
                "kind": kind,
                "summary": summary,
                "detail": detail,
                "ok": ok,
                "ms": int(ms),
                "changed": list(changed or []),
                "picture": picture or "",
            })
            self.publish()

    def task_json(self, index, plan_task):
        return {"index": index, "title": plan_task["title"], "detail": plan_task["detail"],
                "needs": plan_task.get("needs", ""), "files": plan_task.get("files", []),
                "after": plan_task.get("after", []), "expert": "", "state": "waiting",
                "outcome": "", "started_unix": 0, "ended_unix": 0}

    # --- the core's switches ---------------------------------------------------

    def stopping(self):
        flags = self.core.call("engine.flags") or {}
        return bool(flags.get("stop") or flags.get("cancel") or not flags.get("running", True))

    def canceled(self):
        flags = self.core.call("engine.flags") or {}
        return bool(flags.get("cancel") or not flags.get("running", True))

    # --- the seats ------------------------------------------------------------------

    def take_architect(self):
        """The seat that plans and reviews: the one the settings name, or the
        one the delegator picks for the directive."""
        pinned = self.settings.get("architect") or ""
        seats = self.params.get("seats") or {}
        if pinned and not (seats.get(pinned) or {}).get("model"):
            pinned = ""
        return cook.take_seat(self.core, self.params, self.roster,
                              "Plan and oversee a software project: " + self.directive, pinned)

    def decide_task(self, task):
        """Who should do `task`: the delegator's pick, or a seat made for it.

        A task's "needs" is what is routed on, with the title and detail
        beside it: a delegator shown "CSS layout" alone leans on the word, and
        shown the detail alone leans on the project. When the pick was nobody
        in particular -- a fallback, or a guess below the confidence floor --
        and a model for new agents is configured, a seat is made and the task
        pinned to it, so the next task that needs the same thing finds it.
        """
        needs = task.get("needs") or ""
        work = ((needs + ": ") if needs else "") + task["title"] + "\n" + task["detail"]
        # A seat already named for exactly this expertise is the obvious pick,
        # whatever the delegator thinks: it was made for it, perhaps by this
        # build a task ago.
        for expert in self.roster.experts:
            if needs and expert.get("name", "").strip().lower() == needs.strip().lower():
                return routing.Decision(expert.get("id", ""), 1.0, routing.PINNED, "a seat named for it")
        decision = cook.decide(self.core, self.params, work)
        unsure = (decision.source == routing.FALLBACK or not decision.expert
                  or decision.detail.startswith("undecided"))
        if unsure and needs and self.settings.get("can_make"):
            made = self.make_seat(needs, task)
            if made is not None:
                decision = routing.Decision(made, 1.0, routing.PINNED, "made for this build")
        return self.mixed(decision, task, work)

    def mixed(self, decision, task, work):
        """Frontier models for the heavy lifting and models on this machine for
        the small tasks, when the roster has both: a task the plan called large
        that the delegator gave to a local seat goes to a provider's instead,
        and a small one it gave to a provider goes to a local seat -- each
        chosen by the delegator from among its own kind, so expertise still
        decides which. What it saves is the provider's tokens on the work a
        local model does as well."""
        if not self.settings.get("split", True) or not decision.expert:
            return decision
        size = task.get("size") or ""
        seats = self.params.get("seats") or {}
        remote = {i for i, s in seats.items() if s.get("model") and s.get("remote")}
        local = {i for i, s in seats.items() if s.get("model") and not s.get("remote")}
        if not remote or not local:
            return decision
        chosen_remote = decision.expert in remote
        if size == "large" and not chosen_remote:
            kind, wanted = "a provider's model", remote
        elif size == "small" and chosen_remote:
            kind, wanted = "a model on this machine", local
        else:
            return decision
        architect = self.settings.get("architect") or ""
        if kind.startswith("a provider") and architect in wanted:
            return routing.Decision(architect, 1.0, routing.PINNED, "a large task, for the architect")
        # The delegator again, among those seats alone.
        narrowed = dict(self.params)
        narrowed["roster"] = [e for e in (self.params.get("roster") or []) if e.get("id") in wanted]
        again = cook.decide(self.core, narrowed, work)
        expert = again.expert if again.expert in wanted else sorted(wanted)[0]
        return routing.Decision(expert, again.confidence, routing.PINNED,
                                "a %s task, for %s" % (size, kind))

    def decision_for(self, index):
        """The decision for a task, made once and kept: the scheduler asks
        each time it looks at a task it could not yet start."""
        if index not in self.decisions:
            self.decisions[index] = self.decide_task(self.journal["tasks"][index])
        return self.decisions[index]

    def local_of(self, expert_id):
        """The model on this machine that answers for a seat, or "" for a
        provider's."""
        return ((self.params.get("seats") or {}).get(expert_id) or {}).get("local", "")

    def make_seat(self, needs, task):
        """Add a seat named for `needs` to the roster, on the model configured
        for new agents. Returns its id, or None when the core would not."""
        name = " ".join(w[:1].upper() + w[1:] for w in needs.split()[:4])
        # The expertise and nothing of the task: the words of the blurb are
        # what the keyword router matches on, and a blurb that carried the
        # task's own words -- "notes", "database" -- made the seat take every
        # later task that mentioned the project, whatever it needed.
        blurb = (needs + ": an expert a build made for the tasks that need " + needs.lower()
                 + ". It takes questions and work of that kind.")
        reply = self.core.call("roster.make", {"name": name, "blurb": blurb}) or {}
        if not reply.get("ok"):
            self.note("note", "could not add an expert for " + needs + ": "
                      + str(reply.get("error", "")), ok=False)
            return None
        expert_id = reply.get("id", "")
        entry = {"id": expert_id, "name": reply.get("name", name), "tag": reply.get("tag", ""),
                 "blurb": blurb, "keywords": reply.get("keywords") or [], "examples": []}
        with self.lock:
            # Copies, not appends: the lists came in with the request, and a
            # seat made for this build must not reach into whoever else holds
            # them -- an agent reading them as they change, among others.
            self.params["roster"] = list(self.params.get("roster") or []) + [entry]
            seats = dict(self.params.get("seats") or {})
            seats[expert_id] = {"model": True, "remote": bool(reply.get("remote")),
                                "local": reply.get("local", "")}
            self.params["seats"] = seats
            self.roster = routing.Roster(self.params.get("roster"))
            self.made.append(expert_id)
        self.note("note", "added " + entry["name"] + " to the experts, for " + needs,
                  expert=expert_id)
        return expert_id

    def attachments_for(self, seat, system):
        """What the directive came with, read for `seat` -- its context is the
        size the share is of, and whether it sees pictures is its own. None
        when nothing was attached."""
        if not self.params.get("has_attachments"):
            return None
        composed = self.core.call("seat.attachments",
                                  {"seat": seat.handle, "system": system,
                                   "share": cook.ATTACHED_SHARE}) or {}
        return {"role": "user",
                "content": "The directive comes with these attached:\n\n" + composed.get("text", ""),
                "attached": True}

    def round(self, seat, messages):
        return self.core.call("seat.chat", {"seat": seat.handle, "messages": messages}) or {}

    def run_tool(self, call, seat=None):
        asked = dict(call)
        asked["seat"] = (seat or self.seat).handle
        return self.core.call("tools.run", asked) or {}

    # --- before the plan --------------------------------------------------------------

    def survey(self):
        """What is in the project, for the architect: the listing, and the
        files that say what a project is when it already is one."""
        self.machine = machine_line(self.core.call("machine.facts") or {})
        listing = self.run_tool({"kind": "list", "argument": ".", "content": ""})
        self.listing = listing.get("output", "")
        names = {line.strip() for line in self.listing.splitlines()}
        read = []
        for manifest in MANIFESTS:
            if manifest not in names:
                continue
            got = self.run_tool({"kind": "read", "argument": manifest, "content": ""})
            if got.get("ok"):
                read.append(manifest + ":\n" + cook.clip(got.get("output", ""), MANIFEST_CHARS))
        empty = "(empty)" in self.listing
        self.was_empty = empty
        text = ("It is empty: everything is to be made." if empty
                else "Here is what is in it:\n\n" + self.listing)
        if read:
            text += "\n\n" + "\n\n".join(read)
        return text

    def plan(self):
        """Ask the architect for a plan, and the person whether to run it.

        The plan is put up for the person before anything is made: a build is
        an hour of somebody else's work on their project, and a wrong plan
        caught here costs a sentence rather than the hour.
        """
        survey = self.survey()
        feedback = ""
        for revision in range(PLAN_REVISIONS + 1):
            plan, why = self.ask_for_plan(survey, feedback)
            if plan is None:
                self.note("note", "the architect wrote no usable plan: " + why, ok=False)
                plan = {"summary": "", "run": "", "check": "", "ship": "",
                        "tasks": [{"title": cook.clip(self.directive, 60), "detail": self.directive,
                                   "files": [], "needs": "", "after": []}]}
            if len(plan["tasks"]) > 1 and (ONE_TASK.search(self.directive) or ONE_TASK.search(feedback)):
                plan = one_task(plan, self.directive)
            self.adopt_plan(plan)
            # A plan of one task is a change to make, not a project to agree:
            # "make the button blue" asked whether to make the button blue
            # would be a question nobody wants.
            if (not self.settings.get("confirm_plan", True) or revision == PLAN_REVISIONS
                    or len(plan["tasks"]) == 1):
                return True
            answer = self.ask("The plan has %d %s. Reply go to start it, or say what to change."
                              % (len(plan["tasks"]), "task" if len(plan["tasks"]) == 1 else "tasks"))
            if answer is None:
                return False   # stopped while waiting
            if answer.strip().lower().rstrip(".!") in GO_WORDS:
                return True
            feedback = answer
            self.note("note", "the plan is being changed: " + cook.clip(answer, 120))
        return True

    def ask_for_plan(self, survey, feedback):
        messages = [{"role": "system", "content": self.params.get("system_prompt", "")}]
        attached = self.attachments_for(self.seat, messages[0]["content"])
        if attached is not None:
            messages.append(attached)
            messages.append({"role": "assistant", "content": "Read. I will plan from these."})
        prompt = plan_prompt(self.directive, self.root, survey, feedback, self.machine)
        messages.append({"role": "user", "content": prompt})
        why = ""
        text = ""
        for attempt in range(PLAN_RETRIES + 1):
            self.core.mood("thinking", self.seat.name + " is planning")
            reply = self.round(self.seat, messages)
            if reply.get("error"):
                return None, reply["error"]
            text = reply.get("answer", "") or reply.get("reasoning", "")
            plan, why = read_plan(text)
            if plan is not None:
                self.note("plan", "planned %d %s" % (len(plan["tasks"]),
                                                      "task" if len(plan["tasks"]) == 1 else "tasks"),
                          detail=text, ms=reply.get("ms", 0))
                self.planned_from = (prompt, text)
                return plan, ""
            # What it wrote instead, kept where a person can open it: the
            # difference between a model that wrote prose and one that wrote
            # JSON with a mistake in it is the difference between changing the
            # architect and changing the reader.
            self.note("think", "the plan could not be read: " + why, ok=False, detail=text,
                      ms=reply.get("ms", 0))
            messages.append({"role": "assistant", "content": text})
            messages.append({"role": "user", "content": "That could not be used: " + why
                             + ". Reply with only the JSON, in the shape given."})
        return None, why

    def adopt_plan(self, plan):
        with self.lock:
            self.journal["plan"] = {k: plan[k] for k in ("summary", "run", "check", "ship")}
            self.journal["tasks"] = [self.task_json(i, t) for i, t in enumerate(plan["tasks"])]
            self.decisions = {}
            self.publish()

    def ask(self, question, expert=None, task=-1):
        """Put a question to the person and wait. None when the build was
        stopped. One question at a time: the journal holds one, and the
        answer typed is to that one."""
        with self.ask_lock:
            with self.lock:
                self.journal["state"] = "asking"
                self.journal["question"] = question
            self.note("ask", "asked: " + question, task=task, expert=expert)
            self.core.mood("idle", "waiting for your answer")
            answered = (self.core.call("cook.await_answer") or {}).get("answer")
            with self.lock:
                self.journal["question"] = ""
                self.journal["state"] = "working"
                self.publish()
            return answered

    # --- one task ---------------------------------------------------------------------

    def account(self):
        """What the other agents have done, for the top of a task's window."""
        with self.lock:
            lines = []
            for task in self.journal["tasks"]:
                if task["state"] in ("done", "incomplete", "failed"):
                    lines.append("- task %d, %s (%s): %s" % (task["index"] + 1, task["title"],
                                                              task["state"],
                                                              cook.clip(task["outcome"] or "no account", 200)))
                elif task["state"] == "working":
                    lines.append("- task %d, %s: being worked on by another agent right now"
                                 % (task["index"] + 1, task["title"]))
            files = cook.files_touched(self.journal)
        text = ""
        if lines:
            text += "What the other agents have done so far:\n" + "\n".join(lines) + "\n"
        if files:
            text += "Files changed in this build so far: " + ", ".join(files) + "\n"
        return text

    def task_files(self, index):
        """The files task `index` changed, first-touched order."""
        with self.lock:
            seen, files = set(), []
            for step in self.journal["steps"]:
                if step.get("task") != index:
                    continue
                for path in step.get("changed") or []:
                    if path not in seen:
                        seen.add(path)
                        files.append(path)
            return files

    def run_task(self, index, decision=None, rounds=None):
        """Work one task until it says DONE, runs out of rounds, or stalls.

        The loop is the cook's, for a task rather than a goal: the same tools,
        the same nudges for a nearly right command, the same noticing of a
        model going in circles. What differs is how it ends -- DONE ends the
        task -- and that HANDOFF changes who holds this task rather than what
        the work is. It may run beside other tasks, on a thread of its own:
        everything it shares goes through the build's lock, and its seat is
        its own.

        Returns the task's final state.
        """
        with self.lock:
            task = self.journal["tasks"][index]
            plan = self.journal["plan"]
            count = len(self.journal["tasks"])
            task["state"] = "working"
            task["started_unix"] = int(time.time())
            self.journal["iterations"] += 1
            self.publish()
        rounds = rounds or self.rounds

        if decision is None:
            decision = self.decide_task(task)
        seat = cook.seat_decision(self.core, self.params, self.roster, decision)
        if not seat.ok:
            with self.lock:
                task["state"] = "failed"
                task["outcome"] = seat.error
                task["ended_unix"] = int(time.time())
            self.note("note", task["title"] + ": " + seat.error, task=index, ok=False, expert=seat.id)
            return "failed"
        # Whoever holds the task when it ends -- a HANDOFF changes it -- hands
        # the seat back, however it ended.
        holder = {"seat": seat}
        try:
            state = self.work_on(index, task, plan, count, holder, rounds)
        finally:
            holder["seat"].release(self.core)
        if state == "done":
            self.record(task)
            self.commit(task)
        return state

    def work_on(self, index, task, plan, count, holder, rounds):
        """The rounds of one task, with holder["seat"] holding it -- kept up
        to date through a HANDOFF, so the caller releases the right seat."""
        seat = holder["seat"]
        with self.lock:
            task["expert"] = seat.id
            self.publish()

        def system_for():
            return task_system_prompt(self.params.get("system_prompt", ""), self.directive, plan,
                                      task, index, count, self.root, self.account(),
                                      self.params.get("tools", ""), self.machine)

        system = system_for()
        attached = self.attachments_for(seat, system)
        listing = self.run_tool({"kind": "list", "argument": ".", "content": ""}, seat).get("output", "")
        instruction = ("Here is what is in the project now:\n\n" + listing
                       + "\nStart on your task. Read whichever of these files it concerns "
                         "first; only these files exist.")
        recent, window = [], []
        idle = strikes = 0
        pictures = None      # the message the last tool's pictures go with
        state = "incomplete"
        wrote = False        # whether any action changed a file
        pushed_back = False  # DONE refused once, when nothing was made

        for _ in range(rounds):
            if self.stopping():
                state = "stopped"
                break
            messages = [{"role": "system", "content": system}]
            if attached is not None:
                messages.append(attached)
                messages.append({"role": "assistant", "content": "Read. I will work from these."})
            messages.extend(recent[-RECENT_TURNS * 2:])
            messages.append({"role": "user", "content": instruction, "tool_pictures": pictures is not None})
            pictures = None

            self.core.mood("thinking", seat.name + " is working on task " + str(index + 1), linked=seat.id)
            reply = self.round(seat, messages)
            if self.canceled():
                state = "stopped"
                break
            if reply.get("error"):
                self.note("note", reply["error"], task=index, ok=False, ms=reply.get("ms", 0), expert=seat.id)
                with self.lock:
                    task["outcome"] = reply["error"]
                state = "failed"
                break

            answer = reply.get("answer", "")
            reasoning = reply.get("reasoning", "")
            parsed = self.core.call("tools.parse", {"answer": answer, "reasoning": reasoning}) or {}
            call = parsed.get("call")
            if call or answer.strip():
                recent.append({"role": "user", "content": instruction})
                recent.append({"role": "assistant", "content": answer or "(no reply)"})

            if not call:
                said, thought = answer.strip(), reasoning.strip()
                self.note("think", cook.clip(said, 200) if said
                          else "(only thought) " + cook.clip(thought, 180) if thought
                          else "(said nothing)", task=index, ms=reply.get("ms", 0), expert=seat.id)
                idle += 1
                if idle >= IDLE_LIMIT:
                    self.note("note", "stopped the task: %s answered %d times in a row without "
                                      "taking an action" % (seat.name, IDLE_LIMIT), task=index, ok=False,
                              expert=seat.id)
                    with self.lock:
                        task["outcome"] = "the agent kept answering without acting"
                    break
                instruction = cook.nudge_for(parsed)
                continue
            kind = call.get("kind", "")
            argument = call.get("argument", "")

            if kind == "note":
                # Recorded, and not progress: a model that narrates instead of
                # acting is idle, and is told plainly to act.
                self.note("note", argument, task=index, ms=reply.get("ms", 0), expert=seat.id)
                idle += 1
                if idle >= IDLE_LIMIT:
                    self.note("note", "stopped the task: %s wrote notes and took no action" % seat.name,
                              task=index, ok=False, expert=seat.id)
                    with self.lock:
                        task["outcome"] = "the agent kept answering without acting"
                    break
                instruction = NOTE_PUSH
                continue
            idle = 0

            if kind == "ask":
                answered = self.ask(argument, expert=seat.id, task=index)
                if answered is None:
                    state = "stopped"
                    break
                recent.append({"role": "user", "content": "The user answered: " + answered})
                instruction = "Carry on with that in mind."
                continue

            if kind == "done":
                # A small model says DONE to a task it has not started. When
                # the plan named files and none was written, it is told so
                # once; a second DONE is taken at its word, and the review
                # says what was and was not made.
                if not wrote and task.get("files") and not pushed_back:
                    pushed_back = True
                    self.note("note", "said DONE without writing anything -- asked to do the task",
                              task=index, ok=False, ms=reply.get("ms", 0), expert=seat.id)
                    instruction = ("Nothing has been written yet, and this task is expected to make "
                                   + ", ".join(task["files"]) + ". Do the work now -- WRITE the file "
                                   "with its complete contents -- and say DONE only when it exists.")
                    continue
                with self.lock:
                    task["outcome"] = argument or "finished"
                self.note("done", argument or "finished the task", task=index, ms=reply.get("ms", 0),
                          expert=seat.id)
                state = "done"
                break

            if kind == "handoff":
                # Somebody else should have this task. Routed on what the agent
                # said it needs, and the task carries on with whoever that is;
                # the new seat starts from the account, not the old one's turns.
                self.note("handoff", argument or "needs a different expert", task=index,
                          ms=reply.get("ms", 0), expert=seat.id)
                decision = cook.decide(self.core, self.params, argument or task["detail"])
                if decision.expert and decision.expert != seat.id:
                    # Let go first: the next expert's model may need the room.
                    seat.release(self.core)
                    other = cook.seat_decision(self.core, self.params, self.roster, decision)
                    if not other.ok:
                        other = cook.seat_decision(self.core, self.params, self.roster,
                                                   routing.Decision(seat.id, 1.0, routing.PINNED,
                                                                    "carrying on"))
                        if not other.ok:
                            with self.lock:
                                task["outcome"] = other.error or seat.name + " could not be loaded again"
                            state = "failed"
                            break
                        instruction = "Nobody else can take it. Carry on with the task yourself."
                    else:
                        self.note("note", seat.name + " handed task " + str(index + 1) + " to "
                                  + other.name, task=index, expert=other.id)
                        instruction = "Take over this task: " + (argument or task["detail"])
                        recent = []
                    seat = holder["seat"] = other
                    with self.lock:
                        task["expert"] = seat.id
                        self.publish()
                    system = system_for()
                    attached = self.attachments_for(seat, system)
                else:
                    instruction = "Nobody else can take it. Carry on with the task yourself."
                continue

            if kind in ("write", "edit") and not self.core.flags().get("auto_edits"):
                approved = (self.core.call("edit.ask", call) or {}).get("approved", False)
                if self.canceled():
                    state = "stopped"
                    break
                if not approved:
                    self.note("note", "you declined the edit to " + argument, task=index, ok=False,
                              expert=seat.id)
                    recent.append({"role": "user", "content": "The user declined that edit; the file is unchanged."})
                    instruction = ("The user declined that edit, so the file is unchanged. Do not "
                                   "try the same write again: change it, or do something else.")
                    continue
                self.core.mood("thinking", seat.name + " is working on task " + str(index + 1))

            started = time.monotonic()
            result = self.run_tool(call, seat)
            changed = result.get("changed") or []
            wrote = wrote or bool(changed)
            self.note(kind, result.get("summary", ""), task=index, ok=result.get("ok", False),
                      detail=result.get("detail", ""), ms=(time.monotonic() - started) * 1000,
                      changed=changed, picture=result.get("picture", ""), expert=seat.id)
            handback = {"role": "user", "content": result.get("output", "")}
            if result.get("pictures"):
                handback["tool_pictures"] = True
                pictures = handback
            recent.append(handback)
            instruction = "Continue."

            window.append((kind + "|" + argument, bool(changed)))
            window = window[-CYCLE_WINDOW:]
            if len(window) < CYCLE_WINDOW:
                continue
            cycling = (not any(moved for _, moved in window)
                       and len({sig for sig, _ in window}) <= CYCLE_DISTINCT)
            if not cycling:
                strikes = 0
                continue
            strikes += 1
            if strikes >= cook.RESTART_AT:
                self.note("note", "stopped the task: going in circles with nothing changing",
                          task=index, ok=False, expert=seat.id)
                with self.lock:
                    task["outcome"] = "the agent repeated the same actions without progress"
                break
            instruction = ("You are going in circles: the last several actions changed nothing "
                           "and told you nothing new. Do something different -- READ a file you "
                           "have not read, WRITE a change, or say DONE: with what stands.")

        with self.lock:
            task["state"] = state
            task["ended_unix"] = int(time.time())
            if state == "incomplete" and not task["outcome"]:
                task["outcome"] = "ran out of rounds before saying it was done"
            self.publish()
        return state

    # --- several at once -------------------------------------------------------------

    def work_through(self):
        """Work the plan's tasks, as many at once as may run together.

        A task starts when the tasks it comes after are over, no running task
        touches one of its files, no running task is on the same model on
        this machine, and fewer agents are at work than the settings allow.
        One that comes after a task that failed is skipped. Returns True when
        the person stopped the build.
        """
        tasks = self.journal["tasks"]
        pending = order_tasks([{"after": t["after"]} for t in tasks])
        running = {}            # index -> (its files, its local model)
        finished = queue.Queue()
        stopped = False

        def start(index, decision, files, local):
            pending.remove(index)
            running[index] = (files, local)
            self.core.spawn(self.work, index, decision, finished)

        while pending or running:
            if not stopped and self.stopping():
                stopped = True
            if not stopped:
                for index in list(pending):
                    if len(running) >= self.agents:
                        break
                    task = tasks[index]
                    blocked = [a for a in task["after"] if tasks[a]["state"] in ("failed", "skipped", "stopped")]
                    if blocked:
                        with self.lock:
                            task["state"] = "skipped"
                            task["outcome"] = "task %d did not finish" % (blocked[0] + 1)
                        self.note("note", task["title"] + ": skipped, " + task["outcome"], task=index, ok=False)
                        pending.remove(index)
                        continue
                    if any(tasks[a]["state"] not in FINISHED for a in task["after"]):
                        continue   # waiting on a task still to finish
                    files = set(task.get("files") or [])
                    if any(files & theirs for theirs, _ in running.values()):
                        continue   # another agent has one of its files
                    decision = self.decision_for(index)
                    local = self.local_of(decision.expert)
                    if local and any(local == theirs for _, theirs in running.values()):
                        continue   # its model is busy with another agent
                    start(index, decision, files, local)
            if not running:
                if pending and not stopped:
                    # Nothing could start and nothing is running: what is
                    # left waits on itself, which read_plan should have
                    # prevented. The first is started rather than hang.
                    index = pending[0]
                    start(index, self.decision_for(index), set(), "")
                    continue
                break
            index, state = finished.get()
            running.pop(index, None)
            if state == "stopped":
                stopped = True
        return stopped

    def work(self, index, decision, finished):
        """One agent's thread: the task, and word to the scheduler when it is
        over however it ended."""
        state = "failed"
        try:
            state = self.run_task(index, decision)
        except Exception as error:   # the pipe, a bug: the build goes on without the task
            with self.lock:
                task = self.journal["tasks"][index]
                task["state"] = "failed"
                task["outcome"] = "the agent's thread failed: %s" % error
            self.note("note", "task %d failed: %s" % (index + 1, error), task=index, ok=False)
        finally:
            finished.put((index, state))

    # --- after a task ---------------------------------------------------------------

    def record(self, task):
        """Keep what a task's agent was asked and what it made, as a record a
        local expert could later be taught from -- and who did it, as a record
        a delegator could. The core writes them beside the project's history;
        nothing leaves the machine."""
        seats = self.params.get("seats") or {}
        # Who a finished task went to: what a local delegator learns to split
        # the work by. The task as the question, the seat's name as the answer.
        needs = task.get("needs") or ""
        self.core.call("teach.record", {
            "expert": "delegator",
            "prompt": ((needs + ": ") if needs else "") + task["title"] + "\n" + task["detail"],
            "completion": self.roster.label(task["expert"]),
            "files": [],
        })
        if not (seats.get(task["expert"]) or {}).get("remote"):
            return   # a local expert teaching itself its own answers is no lesson
        self.core.call("teach.record", {
            "expert": task["expert"],
            "prompt": task["title"] + "\n" + task["detail"],
            "completion": task["outcome"],
            "files": self.task_files(task["index"]),
        })

    def record_plan(self):
        """Keep the plan a provider's architect wrote, as a record a local
        model could be taught to plan from: the directive and what the project
        held as the question, the plan as the answer."""
        seats = self.params.get("seats") or {}
        if self.planned_from is None or not (seats.get(self.seat.id) or {}).get("remote"):
            return
        prompt, text = self.planned_from
        self.core.call("teach.record", {"expert": "architect", "prompt": prompt, "completion": text,
                                        "files": []})

    def commit(self, task):
        """Commit what a task changed -- its files and no other agent's --
        when the build is set to and git is there."""
        if not self.settings.get("auto_commit"):
            return
        with self.commit_lock:
            if self.git_ready is None:
                ready = self.core.call("git.ready") or {}
                self.git_ready = bool(ready.get("git"))
                if self.git_ready and not ready.get("repo"):
                    made = self.core.call("git.init") or {}
                    self.note("commit" if made.get("ok") else "note",
                              "started a git repository" if made.get("ok")
                              else "could not start a git repository: " + str(made.get("error", "")),
                              task=task["index"], ok=bool(made.get("ok")))
                    self.git_ready = bool(made.get("ok"))
                elif not self.git_ready:
                    self.note("note", "git is not installed, so nothing is committed", ok=False)
            if not self.git_ready:
                return
            paths = self.task_files(task["index"])
            # The subject is the task's title and the body the plan's account
            # of it, not what the agent said last: a small model's parting
            # words can be an apology or a muddle, and a commit outlives the
            # conversation.
            message = task["title"]
            if task["detail"] and task["detail"] != task["title"]:
                message += "\n\n" + cook.clip(task["detail"], 400)
            asked = {"message": message}
            if self.agents > 1:
                # Side by side, only its own: another agent's half-written
                # file is not part of this task.
                asked["paths"] = paths
            reply = self.core.call("git.commit", asked) or {}
            self.note("commit", reply.get("summary") or reply.get("error") or "committed",
                      task=task["index"], ok=bool(reply.get("ok")), detail=reply.get("detail", ""))

    def check(self):
        """Run the plan's check command. True when it passed, or there was none."""
        command = self.journal["plan"].get("check") or ""
        if not command:
            return True, ""
        self.core.mood("thinking", "checking: " + cook.clip(command, 60))
        started = time.monotonic()
        result = self.run_tool({"kind": "run", "argument": command, "content": ""})
        self.note("run", result.get("summary", ""), ok=result.get("ok", False),
                  detail=result.get("detail", ""), ms=(time.monotonic() - started) * 1000)
        return bool(result.get("ok")), result.get("output", "")

    def fix(self, output):
        """One more task: make the check pass. Routed like any other."""
        command = self.journal["plan"].get("check") or ""
        with self.lock:
            index = len(self.journal["tasks"])
            self.journal["tasks"].append(self.task_json(index, {
                "title": "Make the check pass",
                "detail": ("The check command `" + command + "` failed. Find out why from its "
                           "output, fix the cause, and run it again until it passes.\n\nIts output:\n"
                           + cook.clip(output, 3000)),
                "files": [], "needs": "debugging and testing", "after": []}))
            self.publish()
        return self.run_task(index, rounds=FIX_ROUNDS)

    def finishing_pass(self):
        """Leave the project running when the person stops a build mid-task:
        the cook's finishing pass, by the architect, who knows the plan."""
        seat = self.take_architect()
        if not seat.ok:
            return
        self.seat = seat
        try:
            with self.lock:
                self.journal["state"] = "finishing"
                self.publish()
            self.core.mood("thinking", seat.name + " is finishing up")
            instruction = ("Time is up. Do not start anything new. Make only the changes needed "
                           "to leave the project in a working state -- finish a half-made edit, fix "
                           "what you broke, and check it runs. When there is nothing left to repair, "
                           "reply with DONE: and a short account of what was changed and what is left.")
            system = (self.params.get("system_prompt", "")
                      + "\n\nYou were working on a software build whose directive was: " + self.directive
                      + "\n\n" + self.account() + self.params.get("tools", ""))
            for _ in range(cook.FINISHING_ROUNDS):
                if self.canceled():
                    return
                reply = self.round(seat, [{"role": "system", "content": system},
                                          {"role": "user", "content": instruction}])
                if reply.get("error"):
                    return
                answer = reply.get("answer", "")
                parsed = self.core.call("tools.parse",
                                        {"answer": answer, "reasoning": reply.get("reasoning", "")}) or {}
                call = parsed.get("call")
                if not call or call.get("kind") in ("done", "ask"):
                    with self.lock:
                        self.journal["outcome"] = (call or {}).get("argument") or answer
                    return
                started = time.monotonic()
                result = self.run_tool(call, seat)
                self.note(call.get("kind", ""), result.get("summary", ""), ok=result.get("ok", False),
                          detail=result.get("detail", ""), ms=(time.monotonic() - started) * 1000,
                          changed=result.get("changed") or [])
                instruction = (result.get("output", "")
                               + "\n\nAnything else that must be repaired? If not, reply DONE: and a "
                                 "short account of what was changed.")
        finally:
            seat.release(self.core)

    def review(self, checked, check_output):
        """The architect's account of the build, for the person who asked."""
        plan = self.journal["plan"]
        with self.lock:
            lines = ["- task %d, %s: %s -- %s" % (t["index"] + 1, t["title"], t["state"],
                                                  cook.clip(t["outcome"] or "no account", 240))
                     for t in self.journal["tasks"]]
            files = cook.files_touched(self.journal)
        check = ("There was no check command." if not plan.get("check")
                 else "The check `" + plan["check"] + "` " + ("passed." if checked
                                                              else "failed:\n" + cook.clip(check_output, 1500)))
        summary = ("Tasks:\n" + "\n".join(lines) + "\n\nFiles changed: "
                   + (", ".join(files) or "none") + "\n\n" + check)
        architect = self.take_architect()
        if not architect.ok:
            return summary
        self.seat = architect
        try:
            self.core.mood("thinking", architect.name + " is writing up the build")
            prompt = ("The build is over. Here is what happened:\n\n" + summary
                      + "\n\nWrite, for the person who asked for it, what was built, how to run it"
                      + (" (the plan said: " + plan["run"] + ")" if plan.get("run") else "")
                      + ", and what is left or uncertain. Plain prose, a few short paragraphs, no "
                        "headings. Do not call any tool.")
            reply = self.round(architect, [{"role": "system", "content": self.params.get("system_prompt", "")},
                                           {"role": "user", "content": prompt}])
        finally:
            architect.release(self.core)
        text = (reply.get("answer") or "").strip()
        if reply.get("error") or not text:
            return summary
        self.note("review", cook.clip(text, 160), ms=reply.get("ms", 0), expert=architect.id)
        return text

    # --- the whole thing ------------------------------------------------------------------

    def run(self):
        journal = self.journal
        self.publish()
        self.core.mood("routing", "finding an architect")

        self.seat = self.take_architect()
        if not self.seat.ok:
            journal["state"] = "failed"
            journal["outcome"] = self.seat.error
            journal["ended_unix"] = int(time.time())
            self.publish()
            self.core.mood("error", journal["outcome"])
            return

        stopped = False
        checked, output = True, ""
        try:
            planned = self.plan()
            if planned:
                self.record_plan()
        finally:
            # The architect lets go of its model while the agents work: one of
            # them may need the room, and it is seated again for the write-up.
            self.seat.release(self.core)
        if not planned:
            stopped = True
        else:
            stopped = self.work_through()
            if not stopped and not self.canceled():
                checked, output = self.check()
                if not checked and not self.stopping():
                    state = self.fix(output)
                    if state == "stopped":
                        stopped = True
                    elif state == "done":
                        checked, output = self.check()

        with self.lock:
            for task in journal["tasks"]:
                if task["state"] == "waiting":
                    task["state"] = "skipped"
                    task["outcome"] = "the build was stopped first"

        interrupted = self.canceled()
        tasks = journal["tasks"]
        if stopped and not interrupted:
            self.finishing_pass()
        elif not stopped and not interrupted and len(tasks) == 1 and not self.was_empty and checked:
            # A small change to a project that was there: what the agent said
            # it did is the account. A write-up of one changed line is a page
            # nobody reads, and a model call nobody needed.
            journal["outcome"] = tasks[0]["outcome"] if tasks[0]["state"] == "done" else ""
        elif not stopped and not interrupted:
            journal["outcome"] = self.review(checked, output)

        with self.lock:
            journal["state"] = ("stopped" if stopped or interrupted
                                else "failed" if all(t["state"] != "done" for t in journal["tasks"])
                                else "done")
            if not journal["outcome"]:
                journal["outcome"] = "interrupted" if interrupted else "stopped before it finished"
            journal["ended_unix"] = int(time.time())
            self.publish()


def run(core, params):
    Build(core, params).run()
    return {}
