# SPDX-License-Identifier: MIT
"""The cook loop.

A prompt is one question and one answer. A cook is a goal, worked in passes
until it is stopped: the expert reads the project, changes it, runs it, reads
the failure, changes it again, and goes round. What makes that possible is not
a bigger model, it is a loop that feeds the result of each action back in as
the next turn.

Four things here are worth knowing before reading it.

The context is bounded. An hour of build output does not fit in any window,
and a conversation that overflows one does not fail loudly -- it quietly pushes
the goal out and the expert starts working on whatever is left. So the history
is trimmed to the goal, a short account of what has been done, and the last few
exchanges.

The loop is not ended by the model's opinion of its own work. DONE closes a
piece of work and the loop asks for the next one; a cook runs, improving each
pass, until it is stopped -- or until its time is up, when it was given one.

Stopping is not canceling. Stop means "wrap up": the cook stops taking new work
and makes a finishing pass whose whole job is to leave the project in a state
that runs. Stop now is for "stop now".

Everything is journalled as it happens rather than summarized at the end, so a
cook killed at minute fifty can still say what it changed.

The core does what only the core may: it loads the experts, runs the tools
inside the trusted folder, asks the user about an edit, and keeps the journal
that the window draws and History reads. This side decides what happens next.
"""

import os
import re
import shlex
import time

from . import routing

# How many past exchanges stay in the window. Six is two or three actions with
# their results: fewer and the expert forgets the error it was fixing, more and
# a couple of file listings crowd out the goal. The running account carries
# anything older.
RECENT_TURNS = 6

# How many journal lines the running account carries.
ACCOUNT_LINES = 12

# A cook going in circles is not making progress, and a model that would notice
# that itself would not be going in circles, so the loop notices. Over a window
# rather than a counter: a 1.2B expert once alternated a LIST and a RUN that
# could not work -- neither repeated consecutively, forever. If the last few
# actions came from a handful of distinct ones and none changed a file, it is
# cycling. "Changed nothing" keeps edit-test-edit-test, which is also two
# actions repeating, out of it: that is the sound of a cook working.
#
# A strike is counted each time a full window is judged to be cycling, and kept
# across a restart: the restart empties the window, and the loop that reset the
# count there could never reach ABANDON_AT, so a cook stuck for good spun on
# until somebody noticed.
CYCLE_WINDOW = 8     # actions looked at
CYCLE_DISTINCT = 3   # at most this many distinct ones
NUDGE_AT = 1         # say plainly that it is going in circles (the first strike)
RESTART_AT = 4       # throw the context away and restate the goal
ABANDON_AT = 10      # it is stuck; stop rather than burn the time

# How many actions pass before the loop stops and asks what is next. A small
# model may never say DONE, and then a three-hour cook is one expert doing
# everything, which is the thing the expert list is for. The answer is usually
# the seat already there, which costs one routing pass and no reload.
CHECKPOINT_EVERY = 12

# Rounds in a row without an action before the cook stops. A model that cannot
# follow the protocol will not start on the ninth time of asking; this was three
# hundred lines of "(said nothing)" before there was a limit.
IDLE_LIMIT = 8

# The finishing pass's own budget, so "wrap up" cannot itself run forever.
FINISHING_ROUNDS = 6

# A third of the context for what was attached, so the work still has room.
ATTACHED_SHARE = 0.33

# How much of a thought a THINK step keeps, to open beside its one line.
THOUGHT_CHARS = 4000

# The rest after a piece of work that changed nothing, doubled for each one
# after it that changed nothing too, and never longer than REST_MOST. A cook
# runs until it is stopped, and one with nothing left worth doing spun: given
# "add a one-line docstring to each test", gpt-oss did it in the first pass and
# spent the next ones re-reading the same two files and re-running a pytest
# that was not installed. Resting stops nothing: Stop ends a rest at once, and
# a piece that changes something puts the rest back to none.
REST_FIRST = 60
REST_MOST = 30 * 60

HANDOFF_REQUEST = (
    "Say what the next most valuable piece of work towards the goal is, in one "
    "line, as:\n\nHANDOFF: <the next piece of work>\n\nSay nothing else. If it "
    "needs a different kind of expertise than yours, say so plainly in that line.")

# Once the goal as written is met, each piece of work starts from an aim chosen
# for it: one change, small enough to make and check, and inside the goal. A
# piece with nothing to aim at re-read what it had read; told only "the next
# most valuable piece of work", gpt-oss renamed tests it had been asked to give
# docstrings.
AIM_REQUEST = (
    "The goal is met. Choose the next most valuable improvement to what it asks for: one "
    "concrete change, naming the file, that you can make and check in a few steps. Keep to "
    "the goal: do not rename, move or rewrite anything it does not ask about. Say it in one "
    "line, as:\n\nHANDOFF: <the change>\n\nSay nothing else. If nothing is worth changing, "
    "reply DONE: and why.")


def publish_journal(core, journal, sent):
    """Show the core the journal as it is now, with only the steps it has not
    been sent: the whole journal on every step made a long build slower the
    longer it ran. `sent` is how many the core holds; what comes back is how
    many it holds now, which is what the next publish starts from."""
    steps = journal.get("steps") or []
    sent = sent if 0 <= sent <= len(steps) else 0
    body = dict(journal)
    body["steps"] = steps[sent:]
    reply = core.call("cook.publish", {"cook": body, "steps_from": sent}) or {}
    held = reply.get("steps")
    # A core that holds a count other than what was sent lost track (or is
    # one that keeps no count); the next publish sends from what it holds.
    return held if isinstance(held, int) and 0 <= held <= len(steps) else len(steps)


def system_prompt(base, goal, root, tools, aim="", machine="", missing=()):
    """What the expert is told it is doing, over and above its own system prompt:
    the goal, and once it is met, what this piece of work is for."""
    text = (base
            + "\n\nYou are working on a real project on disk, over a long session, one "
              "action at a time. The goal is:\n\n"
            + goal
            + "\n\nThe project is at " + root
            + ("\n" + machine if machine else "")
            + ("\nNot installed here, so do not run: " + ", ".join(missing) + "." if missing else "")
            + "\n\nWork in small steps. Look before you change: read a file before "
              "rewriting it, and run the project to find out whether a change worked "
              "rather than assuming. Keep to what the goal asks: prefer the smallest "
              "change that makes it measurably better, and leave alone what it does not "
              "mention. When a piece of work is finished, say DONE and what you did, and "
              "you will be asked for the next one.")
    if aim:
        text += "\n\nThe goal is met, and this piece of work improves on it. Its aim: " + aim
    return text + tools


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


def files_touched(cook):
    """First-touched order: the story of the cook, not an alphabet."""
    seen, files = set(), []
    for step in cook["steps"]:
        for path in step.get("changed") or []:
            if path not in seen:
                seen.add(path)
                files.append(path)
    return files


def running_account(cook):
    """What the cook has done, for the top of the window: its memory of itself."""
    steps = cook["steps"]
    if not steps:
        return ""
    text = "What you have done so far:\n"
    for step in steps[-ACCOUNT_LINES:]:
        text += "- " + step["summary"] + ("" if step.get("ok", True) else "  (failed)") + "\n"
    files = files_touched(cook)
    if files:
        text += "Files you have changed: " + ", ".join(files) + "\n"
    return text


def clip(text, limit):
    """At most `limit` characters, cut at a word rather than through one."""
    text = text.strip()
    if len(text) <= limit:
        return text
    cut = text[:limit]
    # Between two words is a line break as much as a space: a thought that
    # lists files or code a line at a time can go far without a space, and was
    # cut through a word. A long run with no break at all, a path or some
    # JSON, is cut after its last slash or comma rather than inside a name.
    space = max(cut.rfind(" "), cut.rfind("\n"), cut.rfind("\t"))
    mark = max((m.end() for m in re.finditer(r"[^\w\s]+", cut)), default=-1)
    if space > limit // 2:
        cut = cut[:space]
    elif mark > limit // 2:
        cut = cut[:mark]
    elif space > 0:
        cut = cut[:space]
    return cut.rstrip(" ,.;:\n\t") + "..."


def line(text, limit):
    """`text` as one line of at most `limit` characters, for a step's summary:
    the journal draws a step on a line, and a line break in it is a space there."""
    return clip(" ".join((text or "").split()), limit)


def thought_step(answer, reasoning):
    """A reply that called nothing, as a THINK step: its line, and the whole of
    what it said or thought, to open from the step, when the line is cut."""
    said, thought = answer.strip(), reasoning.strip()
    if said:
        summary, whole, room = line(said, 200), said, 200
    elif thought:
        summary, whole, room = "(only thought) " + line(thought, 180), thought, 180
    else:
        return "(said nothing)", ""
    return summary, clip(whole, THOUGHT_CHARS) if len(" ".join(whole.split())) > room else ""


def pause(seconds):
    """Sleep, as a rest does: its own name, so a test can rest without waiting."""
    time.sleep(seconds)


class Core:
    """The calls a cook makes of the core, named for what they do."""

    def __init__(self, channel):
        self.channel = channel

    def call(self, method, params=None):
        return self.channel.call(method, params)

    def spawn(self, target, *args):
        """Run `target` on a thread of its own that speaks for the same work --
        a build's agent, whose calls the core must know are the build's."""
        return self.channel.spawn(target, *args)

    def flags(self):
        return self.call("engine.flags") or {}

    def mood(self, mood, text="", linked=None):
        params = {"mood": mood, "text": text}
        if linked is not None:
            params["linked"] = linked
        self.call("mood", params)


class Seat:
    """An expert holding a seat in the core: who, and the handle the core
    knows the seat by. Every round, tool and attachment read names the seat,
    so agents working side by side are each asking of their own."""

    __slots__ = ("id", "name", "error", "handle", "remote", "local")

    def __init__(self, expert_id="", name="", error="", handle="", remote=False, local=""):
        self.id = expert_id
        self.name = name
        self.error = error
        self.handle = handle
        self.remote = remote
        self.local = local

    @property
    def ok(self):
        return not self.error

    def release(self, core):
        """Hand the seat back, and with it the hold on its model."""
        if self.handle:
            core.call("seat.release", {"seat": self.handle})
            self.handle = ""


def decide(core, params, work, pinned=""):
    """The delegator's decision for `work`, policy applied: who should have it."""
    return routing.route(core, {
        "prompt": work,
        "pinned": pinned,
        "roster": params.get("roster"),
        "seats": params.get("seats"),
        "routing": params.get("routing"),
    })


def seat_decision(core, params, roster, decision):
    """Seat the expert a decision named, or say why it cannot be."""
    name = roster.label(decision.expert)
    seats = params.get("seats") or {}
    has_model = bool(seats.get(decision.expert, {}).get("model"))
    if not decision.expert or not has_model:
        return Seat(decision.expert, name,
                    "no expert model is configured to cook with" if not name
                    else name + " has no model, and nothing else is configured either")
    taken = core.call("seat.take", {"expert": decision.expert, "name": name}) or {}
    if not taken.get("ok"):
        return Seat(decision.expert, name, taken.get("error", "could not load"))
    return Seat(decision.expert, name, handle=taken.get("seat", ""),
                remote=bool(taken.get("remote")),
                local=(seats.get(decision.expert) or {}).get("local", ""))


def take_seat(core, params, roster, work, pinned=""):
    """Route `work` and make the winner resident. Shared with the build loop,
    which assigns a seat per task the same way a cook assigns one per piece
    of work."""
    return seat_decision(core, params, roster, decide(core, params, work, pinned))


def nudge_for(parsed):
    """What to say to a reply that called nothing, from what it nearly did.

    "Did not try" is not "tried and got the syntax wrong": a model writing
    `WRITE /path "fixed it"` is failing on one character, and is told which.
    """
    attempted = parsed.get("attempted", "none")
    if attempted == "write":
        return ("That was nearly right, but it cannot be run. WRITE needs a colon "
                "after it, and the new contents of the file go in a fenced block "
                "on the following lines -- never on the same line, and never as a "
                "description of the change. Exactly this shape:\n\n"
                "WRITE: path/to/file\n```\n<the complete new contents>\n```\n\n"
                "Try that again.")
    if attempted and attempted != "none":
        verb = attempted.upper()
        return ("That was nearly right, but it cannot be run: %s needs a colon "
                "after it, like `%s: ...`. Write it again with the colon."
                % (verb, verb))
    return ("Take one action now, using exactly one of the commands you "
            "were given, on a line of its own, with its colon.")


# --- the same call again -------------------------------------------------------
#
# A chat turn does not run a call it has already made: the answer would be the
# one it already has, so the model is told that and to use it (the engine's turn
# loop). A cook or a build shows the model only its last few exchanges, so here
# a call is a repeat while the earlier one's result is still among them and
# nothing has changed since: the same verb given the same thing, or the same
# lines of the same file however they were read. Asked to make a header blue
# without being told which file, gpt-oss read the page with `sed -n` and
# grepped it for color names until its rounds ran out.
#
# The first repeat is not run, and is answered with REPEAT_NUDGE. The next one
# in the same piece of work ends the piece, or the task. A change to a file
# starts the count again.

REPEAT_NUDGE = ("You already did exactly that, and its result is above: nothing has changed "
                "since. Use it, and take a different step. Another repeat ends this {}.")

# Commands that only look. A RUN made of nothing else changes nothing, so what
# was seen before it still stands; after any other command, it is seen afresh.
LOOKING = frozenset(("cat", "nl", "head", "tail", "sed", "grep", "egrep", "fgrep", "rg", "ag",
                     "ls", "find", "wc", "tree", "file", "stat", "pwd", "which", "less", "more",
                     "du", "cut", "diff", "cd", "echo"))

# The verbs that only look, whatever they are given.
LOOKS = frozenset(("list", "read", "find", "search", "fetch"))

# The verbs whose answer is how things are at that moment, the screen or what a
# running program has printed, which asking again rightly changes.
MOMENTS = frozenset(("screenshot", "click", "move", "type", "key", "scroll", "start", "stop", "logs"))

# READ's own range, "big.py 120-180", and a sed script that prints lines by
# number: 120,180p, 120p or 120,$p.
READ_RANGE = re.compile(r"^(.*\S)\s+(\d{1,9})\s*-\s*(\d{1,9})$")
SED_LINES = re.compile(r"^(\d{1,9})(?:,(\d{1,9}|\$))?p$")

# How a shell says it has no such program: dash, bash, zsh and cmd.
NOT_FOUND = re.compile(r"(?:^|\n)[^\n:]*:\s*(?:(?:line )?\d+:\s*)?([\w.+-]+): (?:command )?not found"
                       r"|command not found: ([\w.+-]+)"
                       r"|'([^'\n]+)' is not recognized as an internal or external command")


def shell_words(command):
    """`command` in words, with each pipe, redirection and `&&` a word of its
    own; None when a shell could not read it either."""
    try:
        lexer = shlex.shlex(command, posix=True, punctuation_chars=True)
        lexer.whitespace_split = True
        return list(lexer)
    except ValueError:
        return None


def is_joint(word):
    """Whether a word is the shell's punctuation rather than a program's."""
    return bool(word) and set(word) <= set("();<>|&")


def project_path(path, root=""):
    """A file named the way the project's listing names it, relative to the root."""
    path = path.strip()
    base = root.rstrip("/")
    if base and path.startswith(base + "/"):
        path = path[len(base) + 1:]
    while path.startswith("./"):
        path = path[2:]
    return path


def printed_lines(words):
    """What one command of a pipeline prints of a file, as (file, first, last):
    a last of 0 is to the end, a first below 0 is that many lines from the end,
    and the file is None when the lines come down the pipe. None when the
    command does anything else."""
    program = os.path.basename(words[0]) if words else ""
    args = words[1:]
    if program in ("cat", "nl"):
        files = [a for a in args if not a.startswith("-")]
        return (files[0], 0, 0) if len(files) == 1 else None
    if program == "sed":
        if "-n" not in args or any(a.startswith("-i") or a.startswith("--in-place") for a in args):
            return None
        rest = [a for a in args if a not in ("-n", "-e")]
        lines = SED_LINES.match(rest[0]) if 1 <= len(rest) <= 2 else None
        if not lines:
            return None
        first = int(lines.group(1))
        last = first if lines.group(2) is None else 0 if lines.group(2) == "$" else int(lines.group(2))
        return (rest[1] if len(rest) == 2 else None), first, last
    if program in ("head", "tail"):
        count, files, wanted = "10", [], False
        for a in args:
            if wanted:
                count, wanted = a, False
            elif a in ("-n", "--lines"):
                wanted = True
            elif a.startswith("--lines="):
                count = a[len("--lines="):]
            elif re.fullmatch(r"-n\+?\d+|-\d+", a):
                count = a[2:] if a.startswith("-n") else a[1:]
            elif a.startswith("-"):
                return None   # bytes, following a growing file: not lines of it
            else:
                files.append(a)
        if len(files) > 1 or not re.fullmatch(r"\+?\d{1,9}", count):
            return None
        file = files[0] if files else None
        if program == "head":
            return (file, 1, int(count)) if not count.startswith("+") else None
        return (file, int(count[1:]), 0) if count.startswith("+") else (file, -int(count), 0)
    return None


def read_of(call, root=""):
    """The lines of a file a call does nothing but read, as (file, first, last):
    for READ, and for a RUN that prints them with sed -n, cat, nl, head or tail,
    alone or in a pipe. None for any other call."""
    kind = call.get("kind", "")
    argument = (call.get("argument") or "").strip()
    if kind == "read":
        ranged = READ_RANGE.match(argument)
        if ranged:
            return project_path(ranged.group(1), root), int(ranged.group(2)), int(ranged.group(3))
        return project_path(argument, root), 0, 0
    words = shell_words(argument) if kind == "run" else None
    if not words:
        return None
    stages = [[]]
    for word in words:
        if word == "|":
            stages.append([])
        elif is_joint(word):
            return None
        else:
            stages[-1].append(word)
    read = printed_lines(stages[0])
    if read is None or read[0] is None:
        return None
    for stage in stages[1:]:
        narrowed = printed_lines(stage)
        if narrowed is None or narrowed[0] is not None or read[1:] != (0, 0):
            return None
        read = (read[0],) + narrowed[1:]
    return (project_path(read[0], root),) + read[1:]


# gpt-oss's own edit tool, which it runs as a command when it forgets this
# one has none. Told so in the result, with what to use instead: without it,
# it re-read the file it could not change until the repeat guard ended the task.
APPLY_PATCH = re.compile(r"\s*(?:cd\s+\S+\s*&&\s*)?apply_patch\b")
APPLY_PATCH_HINT = ("\n(There is no apply_patch here. Change part of a file with EDIT: <path> and "
                    "SEARCH/REPLACE blocks, or write it whole with WRITE: <path>.)")


def read_hint(call, result, root=""):
    """For a RUN that printed lines of a file in the project and did nothing
    else, a line to go after its result naming the READ that does the same and
    numbers them; for a RUN of apply_patch, which is not here, what to use
    instead; "" otherwise."""
    if call.get("kind") == "run" and APPLY_PATCH.match(call.get("argument") or ""):
        return APPLY_PATCH_HINT
    read = read_of(call, root) if call.get("kind") == "run" and result.get("ok") else None
    if read is None or os.path.isabs(read[0]) or read[0].startswith(".."):
        return ""
    path, first, last = read
    if first > 0 and last >= first:
        return "\n(READ: %s %d-%d reads the same lines, numbered.)" % (path, first, last)
    return "\n(READ: %s reads it numbered, and READ: %s 1-80 just those lines.)" % (path, path)


def not_installed(call, result):
    """The program a RUN reached for that this machine does not have, or ""."""
    if call.get("kind") != "run" or result.get("ok"):
        return ""
    found = NOT_FOUND.search(result.get("output") or "")
    return next((name for name in found.groups() if name), "") if found else ""


def only_looks(call):
    """Whether a call changes nothing, so that what was seen before it stands."""
    kind = call.get("kind", "")
    if kind in LOOKS:
        return True
    words = shell_words(call.get("argument") or "") if kind == "run" else None
    if not words:
        return False
    command = []
    for word in words + [";"]:
        if not is_joint(word):
            command.append(word)
            continue
        if word not in ("|", "||", "&&", ";"):
            return False   # a redirection, a subshell, a program left running
        if command:
            program = os.path.basename(command[0])
            if (program not in LOOKING
                    or (program == "sed" and any(a.startswith("-i") for a in command[1:]))
                    or (program == "find" and any(a in ("-delete", "-exec", "-execdir", "-ok", "-okdir")
                                                  for a in command[1:]))):
                return False
        command = []
    return True


def signature(call, root=""):
    """What makes two calls the same, or None for one that can rightly be made again."""
    kind = call.get("kind", "")
    if kind in MOMENTS:
        return None
    read = read_of(call, root)
    if read is not None:
        return ("read",) + read
    return (kind, (call.get("argument") or "").strip(), (call.get("content") or "").strip())


def spoken(call):
    """A call as the journal names it: the verb, and what it was given."""
    return call.get("kind", "").upper() + ": " + line(call.get("argument") or "", 80)


class Repeats:
    """The calls one piece of work has made since anything last changed, each
    with where its result went among the exchanges the model is shown."""

    def __init__(self, root=""):
        self.root = root
        self.made = {}
        self.strikes = 0

    def clear(self):
        """A new piece of work, with a window of its own: nothing in it is seen yet."""
        self.made = {}
        self.strikes = 0

    def again(self, call, shown_from):
        """Whether `call` was made already, with its result in what the model was
        just shown (its exchanges from `shown_from` on) and nothing changed
        since. Each one is a strike."""
        key = signature(call, self.root)
        at = self.made.get(key) if key is not None else None
        if at is None or at < shown_from:
            return False
        self.strikes += 1
        return True

    def ran(self, call, result, at):
        """`call` was run, and its result is exchange `at`."""
        if result.get("changed"):
            self.made = {}
            self.strikes = 0
        elif not only_looks(call) and not not_installed(call, result):
            self.made = {}
        key = signature(call, self.root)
        if key is not None:
            self.made[key] = at


class Cook:
    def __init__(self, core, params):
        self.core = core
        self.params = params
        self.goal = params.get("goal", "")
        self.root = params.get("root", "")
        self.roster = routing.Roster(params.get("roster"))
        self.journal = {
            "id": params.get("id", ""),
            "goal": self.goal,
            "attachments": params.get("attachments") or [],
            "state": "working",
            "budget_seconds": int(params.get("budget_seconds") or 0),
            "started_unix": int(time.time()),
            "ended_unix": 0,
            "iterations": 0,
            "outcome": "",
            "question": "",
            "steps": [],
        }
        self.seat = Seat()
        self.attached_for = None   # the seat the attachments were last read for
        self.attached = None       # the message carrying them
        # The pieces of work after the first. `met` is whether the goal as
        # written has been done once (DONE was said), `aim` what the piece
        # under way is for, `changed` whether it has changed a file yet, and
        # `empty` how many pieces in a row ended with nothing changed.
        self.met = False
        self.aim = ""
        self.changed = False
        self.empty = 0
        self.resting = 0           # seconds to rest before the next round
        self.machine = ""          # what this machine has, for the prompt
        self.missing = []          # programs a command reached for that are not here

    # --- the journal ------------------------------------------------------------

    def publish(self):
        self.published = publish_journal(self.core, self.journal, getattr(self, "published", 0))

    def note(self, kind, summary, ok=True, detail="", ms=0, changed=None, expert=None, picture=""):
        self.journal["steps"].append({
            "iteration": self.journal["iterations"],
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

    # --- the core's switches ------------------------------------------------------

    def stopping(self):
        flags = self.core.flags()
        return bool(flags.get("stop") or flags.get("cancel") or not flags.get("running", True))

    def canceled(self):
        flags = self.core.flags()
        return bool(flags.get("cancel") or not flags.get("running", True))

    # --- who is in the seat -------------------------------------------------------

    def take_the_seat(self, work, pinned=""):
        """Route `work` and seat the winner.

        Routed like any other prompt, which is the point: the delegator that
        picks an expert for a question picks the expert for the next piece of
        work, from the same roster with the same measured prompt.
        """
        return take_seat(self.core, self.params, self.roster, work, pinned)

    def read_for_seat(self, system):
        """What the goal came with, read for whoever holds the seat.

        Put in front of every round rather than into the trimmed history: a
        specification the cook forgets halfway through is worse than none. Read
        again when the seat changes hands, because the next expert's context is
        another size and it may or may not see pictures.
        """
        if not self.params.get("has_attachments") or self.attached_for == self.seat.id:
            return
        composed = self.core.call("seat.attachments",
                                  {"seat": self.seat.handle, "system": system,
                                   "share": ATTACHED_SHARE}) or {}
        self.attached = {"role": "user",
                         "content": "The goal comes with these attached:\n\n" + composed.get("text", ""),
                         "attached": True}
        self.attached_for = self.seat.id

    def round(self, messages):
        return self.core.call("seat.chat", {"seat": self.seat.handle, "messages": messages}) or {}

    def run_tool(self, call):
        asked = dict(call)
        asked["seat"] = self.seat.handle
        return self.core.call("tools.run", asked) or {}

    # --- one piece of work, and the next --------------------------------------------

    def system(self):
        """The system prompt as it stands: the goal, what this machine lacks,
        and once the goal is met, the aim of the piece under way: the one it
        named, or for a piece begun without naming one, the plain one."""
        aim = self.aim or ("the next most valuable improvement to what the goal asks for"
                           if self.met else "")
        return system_prompt(self.params.get("system_prompt", ""), self.goal, self.root,
                             self.params.get("tools", ""), aim, self.machine, self.missing)

    def next_request(self):
        """What to ask for when a piece of work is over: the next piece towards
        the goal, or once it is met, an aim."""
        return AIM_REQUEST if self.met else HANDOFF_REQUEST

    def piece_over(self):
        """A piece of work has ended. One that changed nothing makes the next
        wait, longer for each in a row; one that changed something ends that."""
        self.empty = 0 if self.changed else self.empty + 1
        self.changed = False
        self.aim = ""
        self.resting = min(REST_MOST, REST_FIRST * 2 ** (self.empty - 1)) if self.empty else 0

    def rest(self, out_of_time):
        """Wait out a rest, a second at a time, for as long as nothing stops it."""
        seconds, self.resting = self.resting, 0
        minutes = max(1, round(seconds / 60))
        self.note("note", "that piece changed nothing: the next starts in %d minute%s"
                  % (minutes, "" if minutes == 1 else "s"))
        self.core.mood("idle", "resting")
        for _ in range(seconds):
            if self.stopping() or out_of_time():
                break
            pause(1)

    # --- the loop -----------------------------------------------------------------

    def run(self):
        core = self.core
        journal = self.journal
        self.publish()

        self.seat = self.take_the_seat(self.goal, self.params.get("pinned") or "")
        if not self.seat.ok:
            journal["state"] = "failed"
            journal["outcome"] = self.seat.error
            journal["ended_unix"] = int(time.time())
            self.publish()
            core.mood("error", journal["outcome"])
            return

        # What is on PATH, as a build's agents are told: a pass that does not
        # know pytest is missing finds out by running it, pass after pass.
        self.machine = machine_line(core.call("machine.facts") or {})
        budget = journal["budget_seconds"]
        deadline = time.monotonic() + budget if budget > 0 else None

        def out_of_time():
            return deadline is not None and time.monotonic() >= deadline

        # The first thing the expert sees is what is actually in the project.
        # Left to "look first", experts invented plausible file names and spent
        # twenty steps failing to open them; one listing removes all of that.
        listing = self.run_tool({"kind": "list", "argument": ".", "content": ""})
        listed = listing.get("output", "")
        instruction = ("Here is what is in the project:\n\n" + listed
                       + "\nStart by reading whichever of these files the goal is about, then "
                         "make your first improvement. Only these files exist -- do not guess "
                         "at others.")
        journal["iterations"] = 1

        recent = []          # the exchanges since the last trim
        window = []          # the last few actions, and whether each changed anything
        repeats = Repeats(self.root)
        strikes = 0
        looping = False
        stalled = False      # stopped for rounds with no action, not a repeated one
        unreachable = ""     # the expert could not be asked at all
        idle = 0
        since_checkpoint = 0

        while not self.stopping() and not out_of_time():
            if self.resting:
                self.rest(out_of_time)
                continue
            system = self.system()
            messages = [{"role": "system", "content": system}]
            self.read_for_seat(system)
            if self.attached is not None:
                messages.append(self.attached)
                messages.append({"role": "assistant", "content": "Read. I will work from these."})
            account = running_account(journal)
            if account:
                messages.append({"role": "user", "content": account})
                messages.append({"role": "assistant", "content": "Understood. Continuing."})
            shown_from = len(recent) - RECENT_TURNS * 2
            messages.extend(recent[-RECENT_TURNS * 2:])
            messages.append({"role": "user", "content": instruction})

            core.mood("thinking", self.seat.name + " is working")
            reply = self.round(messages)
            if self.canceled():
                break
            if reply.get("error"):
                # Not retried: a cook runs for hours unattended, and hammering a
                # provider that is refusing spends somebody's rate limit, or
                # their money, to learn nothing new.
                self.note("note", reply["error"], ok=False, ms=reply.get("ms", 0))
                unreachable = reply["error"]
                break

            answer = reply.get("answer", "")
            reasoning = reply.get("reasoning", "")
            parsed = core.call("tools.parse", {"answer": answer, "reasoning": reasoning}) or {}
            call = parsed.get("call")

            # Only an exchange that said something goes into the history: a
            # model shown itself saying nothing, round after round, takes the hint.
            if call or answer.strip():
                recent.append({"role": "user", "content": instruction})
                recent.append({"role": "assistant", "content": answer or "(no reply)"})

            if not call:
                summary, whole = thought_step(answer, reasoning)
                self.note("think", summary, detail=whole, ms=reply.get("ms", 0))

                idle += 1
                if idle >= IDLE_LIMIT:
                    self.note("note", "stopped: %s answered %d times in a row without taking "
                                      "an action" % (self.seat.name, IDLE_LIMIT), ok=False)
                    looping = stalled = True
                    break

                instruction = nudge_for(parsed)
                continue

            kind = call.get("kind", "")
            argument = call.get("argument", "")

            if kind == "note":
                # Recorded, and not progress: see build.py's NOTE_PUSH.
                self.note("note", argument, ms=reply.get("ms", 0))
                idle += 1
                if idle >= IDLE_LIMIT:
                    self.note("note", "stopped: %s wrote notes and took no action" % self.seat.name, ok=False)
                    looping = stalled = True
                    break
                instruction = ("Noted. A NOTE does nothing on its own -- take the next action now, "
                               "in the same reply as any note.")
                continue
            idle = 0

            if kind == "ask":
                journal["state"] = "asking"
                journal["question"] = argument
                self.note("ask", "asked: " + argument, ms=reply.get("ms", 0))
                core.mood("idle", "waiting for your answer")
                answered = (core.call("cook.await_answer") or {}).get("answer")
                journal["question"] = ""
                journal["state"] = "working"
                self.publish()
                if answered is None:
                    break   # stopped while waiting
                recent.append({"role": "user", "content": "The user answered: " + answered})
                instruction = "Carry on with that in mind."
                continue

            if kind == "handoff":
                work = argument or self.goal
                self.note("handoff", work, ms=reply.get("ms", 0))
                before = self.seat
                # Let go of the model first: the next expert's may need its room.
                before.release(core)
                after = self.take_the_seat(work)
                if not after.ok:
                    # Nobody could take it: the expert that had it carries on --
                    # a worse specialist finishing the job beats no job. Seated
                    # again, since its model may have been freed for the other.
                    self.note("note", after.error + ". Carrying on with " + before.name, ok=False)
                    again = seat_decision(core, self.params, self.roster,
                                          routing.Decision(before.id, 1.0, routing.PINNED, "carrying on"))
                    if not again.ok:
                        unreachable = again.error or before.name + " could not be loaded again"
                        break
                    self.seat = again
                else:
                    if after.id != before.id:
                        self.note("note", before.name + " handed over to " + after.name,
                                  expert=after.id)
                    self.seat = after
                # A new expert has none of the old one's conversation; giving it
                # one would be giving it someone else's turns as its own. The
                # running account carries the history across.
                journal["iterations"] += 1
                recent = []
                repeats.clear()
                since_checkpoint = 0
                if self.met:
                    self.aim = work   # kept in front of it for the whole piece
                instruction = "Do this now: " + work
                continue

            if kind == "done":
                self.note("done", argument or "finished a piece of work", ms=reply.get("ms", 0))
                # A piece of work is finished, not the cook. Asking for the next
                # piece as a HANDOFF line is what lets the roster change hands:
                # the line goes back through the delegator.
                journal["iterations"] += 1
                recent = []
                repeats.clear()
                since_checkpoint = 0
                self.piece_over()
                self.met = True
                instruction = "That piece is done. " + self.next_request()
                continue

            # The same call again, with its answer still in front of the model:
            # told so once, and the piece is over the next time.
            if repeats.again(call, shown_from):
                if repeats.strikes > 1:
                    self.note("note", "ended the piece: the same call again with nothing changed ("
                              + spoken(call) + ")", ok=False, ms=reply.get("ms", 0))
                    journal["iterations"] += 1
                    recent = []
                    repeats.clear()
                    since_checkpoint = 0
                    self.piece_over()
                    instruction = ("That piece is over: you asked again for what you already had. "
                                   + self.next_request())
                    continue
                self.note("note", "not run, the same as before with nothing changed: " + spoken(call),
                          ok=False, ms=reply.get("ms", 0))
                instruction = REPEAT_NUDGE.format("piece of work")
                continue

            # A write waits for a yes when Auto is off, exactly as in a chat
            # turn -- read at the moment of asking, so the button pressed halfway
            # through a cook takes effect from the next write.
            if kind in ("write", "edit") and not core.flags().get("auto_edits"):
                approved = (core.call("edit.ask", call) or {}).get("approved", False)
                if self.canceled():
                    break
                if not approved:
                    self.note("note", "you declined the edit to " + argument, ok=False)
                    recent.append({"role": "user",
                                   "content": "The user declined that edit; the file is unchanged."})
                    instruction = ("The user declined that edit, so the file is unchanged. Do "
                                   "not try the same write again: change it, or do something "
                                   "else towards the goal.")
                    continue
                core.mood("thinking", self.seat.name + " is working")

            started = time.monotonic()
            result = self.run_tool(call)
            changed = result.get("changed") or []
            self.changed = self.changed or bool(changed)
            missing = not_installed(call, result)
            if missing and missing not in self.missing:
                self.missing.append(missing)
            self.note(kind, result.get("summary", ""), ok=result.get("ok", False),
                      detail=result.get("detail", ""), ms=(time.monotonic() - started) * 1000,
                      changed=changed, picture=result.get("picture", ""))
            # A picture the tool made goes with the next round, to a seat
            # that can see one; the core attaches it to this message.
            recent.append({"role": "user",
                           "content": result.get("output", "") + read_hint(call, result, self.root),
                           "tool_pictures": bool(result.get("pictures"))})
            repeats.ran(call, result, len(recent) - 1)
            instruction = "Continue."

            # Has anyone else a better claim on this? Asked on a schedule, so a
            # model that never says DONE does not keep the seat all cook.
            since_checkpoint += 1
            if since_checkpoint >= CHECKPOINT_EVERY:
                since_checkpoint = 0
                instruction = ("Stop and take stock. " + self.next_request()
                               + "\nIf you are the right expert for it, say so in the line and "
                                 "carry on.")
                continue

            # Am I going in circles? Judged only on a full window; until the
            # window refills after a restart there is nothing new to judge.
            window.append((kind + "|" + argument, bool(changed)))
            window = window[-CYCLE_WINDOW:]
            if len(window) < CYCLE_WINDOW:
                continue
            cycling = (not any(moved for _, moved in window)
                       and len({signature for signature, _ in window}) <= CYCLE_DISTINCT)
            if not cycling:
                strikes = 0
                continue
            strikes += 1

            if strikes >= ABANDON_AT:
                looping = True
                self.note("note", "stopped: going in circles with nothing changing", ok=False)
                break
            if strikes >= RESTART_AT:
                # The context is what keeps it there: a window full of the same
                # failures reads as confirmation that this is the job. Thrown
                # away, with the listing again -- the thing it has most likely
                # lost is what the project contains.
                recent, window = [], []
                repeats.clear()
                journal["iterations"] += 1
                since_checkpoint = 0
                self.piece_over()
                instruction = ("Stop. The last several actions changed nothing. Forget them and "
                               "start again from the goal: " + self.goal
                               + "\n\nHere is what is in the project:\n\n" + listed
                               + "\nYour next action must be to READ one of those files.")
            elif strikes >= NUDGE_AT:
                instruction = ("You are going in circles: the last several actions changed "
                               "nothing and told you nothing new. Do something different -- "
                               "READ a file you have not read, or WRITE a change to one.")

        # --- the finishing pass ------------------------------------------------------
        #
        # Why Stop is not Stop now: a cook interrupted mid-edit leaves a project
        # in a state nobody asked for, and this pass gets it back to one that
        # runs. Not after the expert stalled -- it has just shown it will not act.
        interrupted = self.canceled()
        if not interrupted and not unreachable and not stalled:
            journal["state"] = "finishing"
            self.publish()
            core.mood("thinking", self.seat.name + " is finishing up")
            instruction = ("Time is up. Do not start anything new. Make only the changes needed "
                           "to leave the project in a working state -- finish a half-made edit, "
                           "fix what you broke, and check it runs. When there is nothing left to "
                           "repair, reply with DONE: and a short account of what you changed "
                           "overall and what is left to do.")
            self.aim = ""   # the last piece's aim is not this pass's
            system = self.system()
            for _ in range(FINISHING_ROUNDS):
                if self.canceled():
                    break
                messages = [{"role": "system", "content": system}]
                account = running_account(journal)
                if account:
                    messages.append({"role": "user", "content": account})
                    messages.append({"role": "assistant", "content": "Understood."})
                messages.append({"role": "user", "content": instruction})

                reply = self.round(messages)
                if reply.get("error"):
                    unreachable = reply["error"]
                    break
                answer = reply.get("answer", "")
                parsed = core.call("tools.parse",
                                   {"answer": answer, "reasoning": reply.get("reasoning", "")}) or {}
                call = parsed.get("call")
                if not call or call.get("kind") in ("done", "ask"):
                    journal["outcome"] = (call or {}).get("argument") or answer
                    break

                # A write still waits for a yes when Auto is off: finishing up
                # is not a way around asking.
                if (call.get("kind") in ("write", "edit") and not core.flags().get("auto_edits")
                        and not (core.call("edit.ask", call) or {}).get("approved", False)):
                    if self.canceled():
                        break
                    self.note("note", "you declined the edit to " + call.get("argument", ""), ok=False)
                    instruction = ("The user declined that edit, so the file is unchanged. "
                                   "Anything else that must be repaired before this is left "
                                   "alone? If not, reply DONE: and a short account of what you "
                                   "changed.")
                    continue

                started = time.monotonic()
                result = self.run_tool(call)
                self.note(call.get("kind", ""), result.get("summary", ""),
                          ok=result.get("ok", False), detail=result.get("detail", ""),
                          ms=(time.monotonic() - started) * 1000,
                          changed=result.get("changed") or [])
                instruction = (result.get("output", "")
                               + "\n\nAnything else that must be repaired before this is left "
                                 "alone? If not, reply DONE: and a short account of what you "
                                 "changed.")

        flags = core.flags()
        journal["state"] = ("failed" if unreachable
                            else "stopped" if interrupted or flags.get("stop")
                            else "failed" if looping
                            else "done")
        if unreachable:
            # Over whatever the finishing pass had started to say: this is why
            # the cook ended, and the files it changed are listed beside it.
            journal["outcome"] = "stopped: " + unreachable
        if not journal["outcome"]:
            journal["outcome"] = (
                "interrupted" if interrupted
                else "stopped: the expert kept answering without taking an action. Its replies "
                     "are not in the form a cook needs. A different model may follow it better"
                if stalled
                else "stopped: the expert repeated the same action without making progress. "
                     "A larger model may be needed for this goal"
                if looping
                else "finished")
        journal["ended_unix"] = int(time.time())
        self.publish()
        self.seat.release(core)


def run(core, params):
    Cook(core, params).run()
    return {}
