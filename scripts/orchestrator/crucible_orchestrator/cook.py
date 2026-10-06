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

HANDOFF_REQUEST = (
    "Say what the next most valuable piece of work towards the goal is, in one "
    "line, as:\n\nHANDOFF: <the next piece of work>\n\nSay nothing else. If it "
    "needs a different kind of expertise than yours, say so plainly in that line.")


def system_prompt(base, goal, root, tools):
    """What the expert is told it is doing, over and above its own system prompt."""
    return (base
            + "\n\nYou are working on a real project on disk, over a long session, one "
              "action at a time. The goal is:\n\n"
            + goal
            + "\n\nThe project is at " + root
            + "\n\nWork in small steps. Look before you change: read a file before "
              "rewriting it, and run the project to find out whether a change worked "
              "rather than assuming. Prefer the smallest change that makes something "
              "measurably better. When a piece of work is finished, say DONE and what "
              "you did, and you will be asked for the next one."
            + tools)


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
    space = cut.rfind(" ")
    if space > limit // 2:
        cut = cut[:space]
    return cut.rstrip(" ,.;:") + "..."


class Core:
    """The calls a cook makes of the core, named for what they do."""

    def __init__(self, channel):
        self.channel = channel

    def call(self, method, params=None):
        return self.channel.call(method, params)

    def flags(self):
        return self.call("engine.flags") or {}

    def mood(self, mood, text="", linked=None):
        params = {"mood": mood, "text": text}
        if linked is not None:
            params["linked"] = linked
        self.call("mood", params)


class Seat:
    __slots__ = ("id", "name", "error")

    def __init__(self, expert_id="", name="", error=""):
        self.id = expert_id
        self.name = name
        self.error = error

    @property
    def ok(self):
        return not self.error


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

    # --- the journal ------------------------------------------------------------

    def publish(self):
        self.core.call("cook.publish", {"cook": self.journal})

    def note(self, kind, summary, ok=True, detail="", ms=0, changed=None, expert=None):
        self.journal["steps"].append({
            "iteration": self.journal["iterations"],
            "expert": self.seat.id if expert is None else expert,
            "kind": kind,
            "summary": summary,
            "detail": detail,
            "ok": ok,
            "ms": int(ms),
            "changed": list(changed or []),
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
        """Route `work` and make the winner resident.

        Routed like any other prompt, which is the point: the delegator that
        picks an expert for a question picks the expert for the next piece of
        work, from the same roster with the same measured prompt.
        """
        decision = routing.route(self.core, {
            "prompt": work,
            "pinned": pinned,
            "roster": self.params.get("roster"),
            "seats": self.params.get("seats"),
            "routing": self.params.get("routing"),
        })
        name = self.roster.label(decision.expert)
        has_model = bool((self.params.get("seats") or {}).get(decision.expert, {}).get("model"))
        if not decision.expert or not has_model:
            return Seat(decision.expert, name,
                        "no expert model is configured to cook with" if not name
                        else name + " has no model, and nothing else is configured either")
        taken = self.core.call("seat.take", {"expert": decision.expert, "name": name}) or {}
        return Seat(decision.expert, name, "" if taken.get("ok") else taken.get("error", "could not load"))

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
                                  {"system": system, "share": ATTACHED_SHARE}) or {}
        self.attached = {"role": "user",
                         "content": "The goal comes with these attached:\n\n" + composed.get("text", ""),
                         "attached": True}
        self.attached_for = self.seat.id

    def round(self, messages):
        return self.core.call("seat.chat", {"messages": messages}) or {}

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

        system = system_prompt(self.params.get("system_prompt", ""), self.goal, self.root,
                               self.params.get("tools", ""))
        budget = journal["budget_seconds"]
        deadline = time.monotonic() + budget if budget > 0 else None

        def out_of_time():
            return deadline is not None and time.monotonic() >= deadline

        # The first thing the expert sees is what is actually in the project.
        # Left to "look first", experts invented plausible file names and spent
        # twenty steps failing to open them; one listing removes all of that.
        listing = core.call("tools.run", {"kind": "list", "argument": ".", "content": ""}) or {}
        listed = listing.get("output", "")
        instruction = ("Here is what is in the project:\n\n" + listed
                       + "\nStart by reading whichever of these files the goal is about, then "
                         "make your first improvement. Only these files exist -- do not guess "
                         "at others.")
        journal["iterations"] = 1

        recent = []          # the exchanges since the last trim
        window = []          # the last few actions, and whether each changed anything
        strikes = 0
        looping = False
        stalled = False      # stopped for rounds with no action, not a repeated one
        unreachable = ""     # the expert could not be asked at all
        idle = 0
        since_checkpoint = 0

        while not self.stopping() and not out_of_time():
            messages = [{"role": "system", "content": system}]
            self.read_for_seat(system)
            if self.attached is not None:
                messages.append(self.attached)
                messages.append({"role": "assistant", "content": "Read. I will work from these."})
            account = running_account(journal)
            if account:
                messages.append({"role": "user", "content": account})
                messages.append({"role": "assistant", "content": "Understood. Continuing."})
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
                said, thought = answer.strip(), reasoning.strip()
                summary = (clip(said, 200) if said
                           else "(only thought) " + clip(thought, 180) if thought
                           else "(said nothing)")
                self.note("think", summary, ms=reply.get("ms", 0))

                idle += 1
                if idle >= IDLE_LIMIT:
                    self.note("note", "stopped: %s answered %d times in a row without taking "
                                      "an action" % (self.seat.name, IDLE_LIMIT), ok=False)
                    looping = stalled = True
                    break

                # "Did not try" is not "tried and got the syntax wrong": a model
                # writing `WRITE /path "fixed it"` is failing on one character.
                attempted = parsed.get("attempted", "none")
                if attempted == "write":
                    instruction = (
                        "That was nearly right, but it cannot be run. WRITE needs a colon "
                        "after it, and the new contents of the file go in a fenced block "
                        "on the following lines -- never on the same line, and never as a "
                        "description of the change. Exactly this shape:\n\n"
                        "WRITE: path/to/file\n```\n<the complete new contents>\n```\n\n"
                        "Try that again.")
                elif attempted and attempted != "none":
                    verb = attempted.upper()
                    instruction = ("That was nearly right, but it cannot be run: %s needs a colon "
                                   "after it, like `%s: ...`. Write it again with the colon."
                                   % (verb, verb))
                else:
                    instruction = ("Take one action now, using exactly one of the commands you "
                                   "were given, on a line of its own, with its colon.")
                continue

            idle = 0
            kind = call.get("kind", "")
            argument = call.get("argument", "")

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
                after = self.take_the_seat(work)
                if not after.ok:
                    # Nobody could take it: the expert that had it carries on --
                    # a worse specialist finishing the job beats no job. Asked
                    # for again, because a local expert is freed before the next
                    # one is loaded, so the one it had may be gone by now.
                    self.note("note", after.error + " -- carrying on with " + before.name, ok=False)
                    again = core.call("seat.take", {"expert": before.id, "name": before.name}) or {}
                    if not again.get("ok"):
                        unreachable = again.get("error") or before.name + " could not be loaded again"
                        break
                    self.seat = before
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
                since_checkpoint = 0
                instruction = "Do this now: " + work
                continue

            if kind == "done":
                self.note("done", argument or "finished a piece of work", ms=reply.get("ms", 0))
                # A piece of work is finished, not the cook. Asking for the next
                # piece as a HANDOFF line is what lets the roster change hands:
                # the line goes back through the delegator.
                journal["iterations"] += 1
                recent = []
                since_checkpoint = 0
                instruction = "That piece is done. " + HANDOFF_REQUEST
                continue

            # A write waits for a yes when Auto is off, exactly as in a chat
            # turn -- read at the moment of asking, so the button pressed halfway
            # through a cook takes effect from the next write.
            if kind == "write" and not core.flags().get("auto_edits"):
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
            result = core.call("tools.run", call) or {}
            changed = result.get("changed") or []
            self.note(kind, result.get("summary", ""), ok=result.get("ok", False),
                      detail=result.get("detail", ""), ms=(time.monotonic() - started) * 1000,
                      changed=changed)
            recent.append({"role": "user", "content": result.get("output", "")})
            instruction = "Continue."

            # Has anyone else a better claim on this? Asked on a schedule, so a
            # model that never says DONE does not keep the seat all cook.
            since_checkpoint += 1
            if since_checkpoint >= CHECKPOINT_EVERY:
                since_checkpoint = 0
                instruction = ("Stop and take stock. " + HANDOFF_REQUEST
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
                journal["iterations"] += 1
                since_checkpoint = 0
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

                started = time.monotonic()
                result = core.call("tools.run", call) or {}
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
                     "are not in the form a cook needs -- a different model may follow it better"
                if stalled
                else "stopped: the expert repeated the same action without making progress -- "
                     "a larger model may be needed for this goal"
                if looping
                else "finished")
        journal["ended_unix"] = int(time.time())
        self.publish()


def run(core, params):
    Cook(core, params).run()
    return {}
