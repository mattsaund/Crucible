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

Tasks run one at a time. The core has one worker and one local model
resident, and a build that pretended otherwise would be queueing behind
itself. What the window shows is which agent has the work and what each has
done; the journal keeps the whole of it.

Everything is journalled as it happens, like a cook: the plan, every task's
state, every step with the task it belongs to.
"""

import json
import re
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

# What a person types to start the plan as it is.
GO_WORDS = ("", "go", "yes", "y", "ok", "okay", "start", "build", "run", "do it", "proceed",
            "looks good", "fine", "sure", "yep")

PLAN_SHAPE = ('{"summary": "<one paragraph: what will be built and how>",\n'
              ' "run": "<the command that runs it, or \\"\\">",\n'
              ' "check": "<one command that proves it works -- tests or a smoke run -- or \\"\\">",\n'
              ' "ship": "<the command that packages it for people, or \\"\\">",\n'
              ' "tasks": [\n'
              '   {"title": "<a few words>", "detail": "<exactly what to make, and how to tell it is done>",\n'
              '    "files": ["<paths it will create or change>"], "needs": "<the expertise, two to four words>",\n'
              '    "after": [<indices of tasks that must finish first>]}\n'
              ' ]}')


# --- the plan -------------------------------------------------------------------


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
        "run": str(parsed.get("run") or "").strip(),
        "check": str(parsed.get("check") or "").strip(),
        "ship": str(parsed.get("ship") or "").strip(),
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
        plan["tasks"].append({
            "title": title or cook.clip(detail, 60),
            "detail": detail or title,
            "files": [str(f) for f in files if isinstance(f, (str, int, float))][:12],
            "needs": str(entry.get("needs") or "").strip(),
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
            + "\n\nRules. Three to twelve tasks, each one piece of work one person could finish "
              "in an hour, in the order they should happen. The task that makes the project "
              "run at all comes first; one that writes the README or the documentation comes "
              "last. \"detail\" says exactly what to make and how to tell it is done. "
              "\"needs\" names the kind of expertise in two to four words -- \"Python back "
              "end\", \"CSS layout\", \"SQL schema\", \"technical writing\". \"after\" lists "
              "the 0-based indices of the tasks that must finish first, and is usually empty "
              "or the previous task. \"check\" is one command, run from the project folder, "
              "that proves the whole thing works. Prefer tools the project already uses.")
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
            + "\n\nWork in small steps. Read a file before rewriting it, run things to find out "
              "whether they work rather than assuming, and do only this task -- the others "
              "have their own agents. When it is finished, say DONE: and what you made, with "
              "anything the next agent must know."
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
        self.seat = cook.Seat()
        self.attached = None
        self.attached_for = None
        self.listing = ""
        self.git_ready = None   # unknown until the first commit is wanted
        self.made = []          # seats this build added to the roster
        self.machine = ""       # what this machine has, for the prompts

    # --- the journal -------------------------------------------------------------

    def publish(self):
        self.core.call("cook.publish", {"cook": self.journal})

    def note(self, kind, summary, task=-1, ok=True, detail="", ms=0, changed=None, expert=None,
             picture=""):
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

    def seat_for_task(self, task):
        """Who does `task`: the delegator's pick, or a seat made for it.

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
                return cook.seat_decision(self.core, self.params, self.roster,
                                          routing.Decision(expert.get("id", ""), 1.0, routing.PINNED,
                                                           "a seat named for it"))
        decision = cook.decide(self.core, self.params, work)
        unsure = (decision.source == routing.FALLBACK or not decision.expert
                  or decision.detail.startswith("undecided"))
        if unsure and needs and self.settings.get("can_make"):
            made = self.make_seat(needs, task)
            if made is not None:
                decision = routing.Decision(made, 1.0, routing.PINNED, "made for this build")
        return cook.seat_decision(self.core, self.params, self.roster, decision)

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
        # Copies, not appends: the lists came in with the request, and a seat
        # made for this build must not reach into whoever else holds them.
        self.params["roster"] = list(self.params.get("roster") or []) + [entry]
        self.params["seats"] = dict(self.params.get("seats") or {})
        self.params["seats"][expert_id] = {"model": True, "remote": bool(reply.get("remote"))}
        self.roster = routing.Roster(self.params.get("roster"))
        self.made.append(expert_id)
        self.note("note", "added " + entry["name"] + " to the experts, for " + needs,
                  expert=expert_id)
        return expert_id

    def read_for_seat(self, system):
        if not self.params.get("has_attachments") or self.attached_for == self.seat.id:
            return
        composed = self.core.call("seat.attachments",
                                  {"system": system, "share": cook.ATTACHED_SHARE}) or {}
        self.attached = {"role": "user",
                         "content": "The directive comes with these attached:\n\n" + composed.get("text", ""),
                         "attached": True}
        self.attached_for = self.seat.id

    def round(self, messages):
        return self.core.call("seat.chat", {"messages": messages}) or {}

    def run_tool(self, call):
        return self.core.call("tools.run", call) or {}

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
            self.adopt_plan(plan)
            if not self.settings.get("confirm_plan", True) or revision == PLAN_REVISIONS:
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
        self.read_for_seat(messages[0]["content"])
        if self.attached is not None:
            messages.append(self.attached)
            messages.append({"role": "assistant", "content": "Read. I will plan from these."})
        prompt = plan_prompt(self.directive, self.root, survey, feedback, self.machine)
        messages.append({"role": "user", "content": prompt})
        why = ""
        text = ""
        for attempt in range(PLAN_RETRIES + 1):
            self.core.mood("thinking", self.seat.name + " is planning")
            reply = self.round(messages)
            if reply.get("error"):
                return None, reply["error"]
            text = reply.get("answer", "") or reply.get("reasoning", "")
            plan, why = read_plan(text)
            if plan is not None:
                self.note("plan", "planned %d %s" % (len(plan["tasks"]),
                                                      "task" if len(plan["tasks"]) == 1 else "tasks"),
                          detail=text, ms=reply.get("ms", 0))
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
        self.journal["plan"] = {k: plan[k] for k in ("summary", "run", "check", "ship")}
        self.journal["tasks"] = [self.task_json(i, t) for i, t in enumerate(plan["tasks"])]
        self.publish()

    def ask(self, question):
        """Put a question to the person and wait. None when the build was stopped."""
        self.journal["state"] = "asking"
        self.journal["question"] = question
        self.note("ask", "asked: " + question)
        self.core.mood("idle", "waiting for your answer")
        answered = (self.core.call("cook.await_answer") or {}).get("answer")
        self.journal["question"] = ""
        self.journal["state"] = "working"
        self.publish()
        return answered

    # --- one task ---------------------------------------------------------------------

    def account(self):
        """What the other agents have done, for the top of a task's window."""
        lines = []
        for task in self.journal["tasks"]:
            if task["state"] in ("done", "incomplete", "failed"):
                lines.append("- task %d, %s (%s): %s" % (task["index"] + 1, task["title"],
                                                          task["state"],
                                                          cook.clip(task["outcome"] or "no account", 200)))
        files = cook.files_touched(self.journal)
        text = ""
        if lines:
            text += "What the other agents have done so far:\n" + "\n".join(lines) + "\n"
        if files:
            text += "Files changed in this build so far: " + ", ".join(files) + "\n"
        return text

    def run_task(self, index, rounds=None):
        """Work one task until it says DONE, runs out of rounds, or stalls.

        The loop is the cook's, for a task rather than a goal: the same tools,
        the same nudges for a nearly right command, the same noticing of a
        model going in circles. What differs is how it ends -- DONE ends the
        task, and the build moves on -- and that HANDOFF changes who holds this
        task rather than what the work is.

        Returns the task's final state.
        """
        task = self.journal["tasks"][index]
        plan = self.journal["plan"]
        count = len(self.journal["tasks"])
        rounds = rounds or self.rounds

        task["state"] = "working"
        task["started_unix"] = int(time.time())
        self.journal["iterations"] += 1
        self.publish()

        seat = self.seat_for_task(task)
        if not seat.ok:
            task["state"] = "failed"
            task["outcome"] = seat.error
            task["ended_unix"] = int(time.time())
            self.note("note", task["title"] + ": " + seat.error, task=index, ok=False, expert=seat.id)
            return task["state"]
        self.seat = seat
        task["expert"] = seat.id
        self.publish()

        system = task_system_prompt(self.params.get("system_prompt", ""), self.directive, plan,
                                    task, index, count, self.root, self.account(),
                                    self.params.get("tools", ""), self.machine)
        listing = self.run_tool({"kind": "list", "argument": ".", "content": ""}).get("output", "")
        instruction = ("Here is what is in the project now:\n\n" + listing
                       + "\nStart on your task. Read whichever of these files it concerns "
                         "first; only these files exist.")
        recent, window = [], []
        idle = strikes = 0
        pictures = None   # the message the last tool's pictures go with
        state = "incomplete"
        wrote = False       # whether any action changed a file
        pushed_back = False # DONE refused once, when nothing was made

        for _ in range(rounds):
            if self.stopping():
                state = "stopped"
                break
            messages = [{"role": "system", "content": system}]
            self.read_for_seat(system)
            if self.attached is not None:
                messages.append(self.attached)
                messages.append({"role": "assistant", "content": "Read. I will work from these."})
            messages.extend(recent[-RECENT_TURNS * 2:])
            messages.append({"role": "user", "content": instruction, "tool_pictures": pictures is not None})
            pictures = None

            self.core.mood("thinking", seat.name + " is working on task " + str(index + 1), linked=seat.id)
            reply = self.round(messages)
            if self.canceled():
                state = "stopped"
                break
            if reply.get("error"):
                self.note("note", reply["error"], task=index, ok=False, ms=reply.get("ms", 0))
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
                          else "(said nothing)", task=index, ms=reply.get("ms", 0))
                idle += 1
                if idle >= IDLE_LIMIT:
                    self.note("note", "stopped the task: %s answered %d times in a row without "
                                      "taking an action" % (seat.name, IDLE_LIMIT), task=index, ok=False)
                    task["outcome"] = "the agent kept answering without acting"
                    break
                instruction = cook.nudge_for(parsed)
                continue
            idle = 0

            kind = call.get("kind", "")
            argument = call.get("argument", "")

            if kind == "ask":
                answered = self.ask(argument)
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
                              task=index, ok=False, ms=reply.get("ms", 0))
                    instruction = ("Nothing has been written yet, and this task is expected to make "
                                   + ", ".join(task["files"]) + ". Do the work now -- WRITE the file "
                                   "with its complete contents -- and say DONE only when it exists.")
                    continue
                task["outcome"] = argument or "finished"
                self.note("done", argument or "finished the task", task=index, ms=reply.get("ms", 0))
                state = "done"
                break

            if kind == "handoff":
                # Somebody else should have this task. Routed on what the agent
                # said it needs, and the task carries on with whoever that is;
                # the new seat starts from the account, not the old one's turns.
                self.note("handoff", argument or "needs a different expert", task=index,
                          ms=reply.get("ms", 0))
                other = cook.take_seat(self.core, self.params, self.roster, argument or task["detail"])
                if other.ok and other.id != seat.id:
                    self.note("note", seat.name + " handed task " + str(index + 1) + " to " + other.name,
                              task=index, expert=other.id)
                    seat = self.seat = other
                    task["expert"] = other.id
                    system = task_system_prompt(self.params.get("system_prompt", ""), self.directive,
                                                plan, task, index, count, self.root, self.account(),
                                                self.params.get("tools", ""), self.machine)
                    recent = []
                    instruction = "Take over this task: " + (argument or task["detail"])
                    self.publish()
                else:
                    if not other.ok:
                        again = self.core.call("seat.take", {"expert": seat.id, "name": seat.name}) or {}
                        if not again.get("ok"):
                            task["outcome"] = again.get("error") or seat.name + " could not be loaded again"
                            state = "failed"
                            break
                    instruction = "Nobody else can take it. Carry on with the task yourself."
                continue

            if kind == "write" and not (self.core.flags().get("auto_edits")):
                approved = (self.core.call("edit.ask", call) or {}).get("approved", False)
                if self.canceled():
                    state = "stopped"
                    break
                if not approved:
                    self.note("note", "you declined the edit to " + argument, task=index, ok=False)
                    recent.append({"role": "user", "content": "The user declined that edit; the file is unchanged."})
                    instruction = ("The user declined that edit, so the file is unchanged. Do not "
                                   "try the same write again: change it, or do something else.")
                    continue
                self.core.mood("thinking", seat.name + " is working on task " + str(index + 1))

            started = time.monotonic()
            result = self.run_tool(call)
            changed = result.get("changed") or []
            wrote = wrote or bool(changed)
            self.note(kind, result.get("summary", ""), task=index, ok=result.get("ok", False),
                      detail=result.get("detail", ""), ms=(time.monotonic() - started) * 1000,
                      changed=changed, picture=result.get("picture", ""))
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
                          task=index, ok=False)
                task["outcome"] = "the agent repeated the same actions without progress"
                break
            instruction = ("You are going in circles: the last several actions changed nothing "
                           "and told you nothing new. Do something different -- READ a file you "
                           "have not read, WRITE a change, or say DONE: with what stands.")

        task["state"] = state
        task["ended_unix"] = int(time.time())
        if state == "incomplete" and not task["outcome"]:
            task["outcome"] = "ran out of rounds before saying it was done"
        self.publish()
        if state == "done":
            self.record(task)
            self.commit(task)
        return state

    # --- after a task ---------------------------------------------------------------

    def record(self, task):
        """Keep what a task's agent was asked and what it made, as a record a
        local expert could later be taught from. The core writes it beside
        the project's history; nothing leaves the machine."""
        seats = self.params.get("seats") or {}
        if not (seats.get(task["expert"]) or {}).get("remote"):
            return   # a local expert teaching itself its own answers is no lesson
        self.core.call("teach.record", {
            "expert": task["expert"],
            "prompt": task["title"] + "\n" + task["detail"],
            "completion": task["outcome"],
            "files": [f for f in cook.files_touched(self.journal)
                      if any(s.get("task") == task["index"] and f in (s.get("changed") or [])
                             for s in self.journal["steps"])],
        })

    def commit(self, task):
        """Commit what a task changed, when the build is set to and git is there."""
        if not self.settings.get("auto_commit"):
            return
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
        # The subject is the task's title and the body the plan's account of
        # it, not what the agent said last: a small model's parting words can
        # be an apology or a muddle, and a commit outlives the conversation.
        message = task["title"]
        if task["detail"] and task["detail"] != task["title"]:
            message += "\n\n" + cook.clip(task["detail"], 400)
        reply = self.core.call("git.commit", {"message": message}) or {}
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
        the cook's finishing pass, with the seat that had the work."""
        if not self.seat.ok:
            return
        self.journal["state"] = "finishing"
        self.publish()
        self.core.mood("thinking", self.seat.name + " is finishing up")
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
            reply = self.round([{"role": "system", "content": system},
                                {"role": "user", "content": instruction}])
            if reply.get("error"):
                return
            answer = reply.get("answer", "")
            parsed = self.core.call("tools.parse",
                                    {"answer": answer, "reasoning": reply.get("reasoning", "")}) or {}
            call = parsed.get("call")
            if not call or call.get("kind") in ("done", "ask"):
                self.journal["outcome"] = (call or {}).get("argument") or answer
                return
            started = time.monotonic()
            result = self.run_tool(call)
            self.note(call.get("kind", ""), result.get("summary", ""), ok=result.get("ok", False),
                      detail=result.get("detail", ""), ms=(time.monotonic() - started) * 1000,
                      changed=result.get("changed") or [])
            instruction = (result.get("output", "")
                           + "\n\nAnything else that must be repaired? If not, reply DONE: and a "
                             "short account of what was changed.")

    def review(self, checked, check_output):
        """The architect's account of the build, for the person who asked."""
        plan = self.journal["plan"]
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
        self.core.mood("thinking", architect.name + " is writing up the build")
        prompt = ("The build is over. Here is what happened:\n\n" + summary
                  + "\n\nWrite, for the person who asked for it, what was built, how to run it"
                  + (" (the plan said: " + plan["run"] + ")" if plan.get("run") else "")
                  + ", and what is left or uncertain. Plain prose, a few short paragraphs, no "
                    "headings. Do not call any tool.")
        reply = self.round([{"role": "system", "content": self.params.get("system_prompt", "")},
                            {"role": "user", "content": prompt}])
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
        if not self.plan():
            stopped = True
        else:
            order = order_tasks([{"after": t["after"]} for t in journal["tasks"]])
            for index in order:
                if self.stopping():
                    stopped = True
                    break
                task = journal["tasks"][index]
                waiting_on = [a for a in task["after"]
                              if journal["tasks"][a]["state"] in ("failed", "skipped", "stopped")]
                if waiting_on:
                    task["state"] = "skipped"
                    task["outcome"] = "task %d did not finish" % (waiting_on[0] + 1)
                    self.note("note", task["title"] + ": skipped, " + task["outcome"], task=index, ok=False)
                    continue
                state = self.run_task(index)
                if state == "stopped":
                    stopped = True
                    break

            if not stopped and not self.canceled():
                checked, output = self.check()
                if not checked and not self.stopping():
                    state = self.fix(output)
                    if state == "stopped":
                        stopped = True
                    elif state == "done":
                        checked, output = self.check()

        for task in journal["tasks"]:
            if task["state"] == "waiting":
                task["state"] = "skipped"
                task["outcome"] = "the build was stopped first"

        interrupted = self.canceled()
        if stopped and not interrupted:
            self.finishing_pass()
        elif not stopped and not interrupted:
            journal["outcome"] = self.review(checked, output)

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
