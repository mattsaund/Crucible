# SPDX-License-Identifier: MIT
"""A prompt, worked in one loop until what it asked for is done.

Whatever is typed into Chat (or into the Test window in Create) is routed,
the expert that wins is seated, and it works. Ordinarily that is one round and
the end of it: a question, and its answer. When the expert reaches for a tool
instead (READ a file, WRITE one, RUN the tests, SEARCH the web) the tool is run,
what came of it goes back, and the expert goes round again, an action a round,
until it writes an answer for the person who asked. There is no other mode: a
request to make an app is worked the same way as a question about one, for as
long as it takes.

Six things are worth knowing before reading it.

The expert calls tools the way it was trained to. A model on this machine whose
chat template can write tools is offered them as functions and its calls are
read by the parser that goes with the template (tools/native_tools.hpp): told
to write `READ: file` lines instead, Qwen and flint fell back on their own
<tool_call> partway through and the turn ended on the bare tag. A provider's
model, or one whose template has no tools, writes the lines.

The transcript is the person's, and it is kept readable while the turn works.
A round that reached for a tool said two things: a sentence for the reader and
a call for the machine. The call stays off the reply and the sentence stays on
it (it is the only explanation of the change that will ever be written) and
what the tool did goes above the reply as an action, with the diff it made or
the output it printed.

The expert can change mid-prompt. When the next step needs another kind of
expertise, the expert says so with a HANDOFF line, and every few actions the
loop asks it, out of sight, what the next step is. Either way the delegator
routes that step, and when another expert fits it better, that one is seated
and carries on with the same conversation. A prompt sent to one expert stays
with it.

The context is bounded, and the loop is not. What the expert is shown only
grows, so that each round reads nothing but what is new: the model keeps what
it has already read, and a prompt that changes near its start is read again
whole. Once it nears what the context holds, the older exchanges go at one
stroke and an account of them takes their place beside the request. So a prompt
can take sixty actions without its request being pushed out of the window by
the files it read along the way, and without paying for them every round.

A prompt that asks for something to be made or changed is checked before it
ends. An expert will say it is finished with a page written and the README
still to do, or show the change in its reply and write nothing, so the turn
that is about to end is asked, out of sight, whether all of it is done. It
either says so or carries on with the tools.

The loop is honest about itself. The same call made again with nothing changed
since is not run, a run of actions that goes round the same few steps is told
so, a reply that nearly called a tool is told what was wrong with it, and one
that starts repeating itself is stopped. A prompt has a ceiling on its actions
(Settings, Tools), as a backstop rather than a budget, and the round after it
answers from what it has.

The core does what only the core may: it seats the expert and holds its model
for the turn, keeps the conversation the turn belongs to and fits it to the
expert's context, streams each round into the transcript as it is written,
runs the tools inside the trusted folder, searches the web, and puts an edit to
the person. Which of those happens, in what order, and what the expert is told
about each, is decided here.
"""

import json
import os
import re
import sys
import time

from . import routing, steps
from .rpc import Disconnected

# How many actions one prompt may take, when Settings does not say. A backstop,
# not a budget: a prompt is worked until what was asked is done, and a model
# that would take more than this is going round, whatever it thinks.
ACTIONS = 60

# How many of the turn's own messages stay when the context is nearly full and
# the older ones go: four actions with their results. Fewer and it forgets the
# error it was fixing. The account carries the rest.
RECENT = 8

# How much of what a conversation may take (PROMPT_SHARE of the context) a
# prompt fills before its older exchanges go. They go all at once and well
# before the end, because each time they do the model reads the rest again.
CROWDED = 0.8

# How many lines the account of what was done carries.
ACCOUNT_LINES = 12

# When the loop asks, out of sight, what the next step is, and lets the
# delegator choose who takes it: once a piece of work is finished, which is an
# action that changed a file the prompt had not changed before, and every
# CHECKPOINT_EVERY actions whatever they did. An expert may never hand off by
# itself (flint, asked for a page and then a README explaining it, wrote both
# without a word), and then a prompt is one expert doing everything, which is
# the thing the roster is for. The answer is usually the seat already there,
# which costs one short round and no reload.
CHECKPOINT_EVERY = 6

# How sure the delegator must be to give the next step to another expert at a
# checkpoint. Above the routing floor, so a coin flip does not swap a 36 GB
# model for another; a handoff the expert asked for needs only the floor.
SWITCH_AT = 0.75

# The most a checkpoint may write, thinking included. It is asked for one line;
# one that is still thinking after this much is left where it was.
CHECKPOINT_TOKENS = 256

# Actions after a takeover before a checkpoint may move the work again, so two
# experts do not pass a step back and forth.
SETTLE = 2

# How many replies in a row may nearly call a tool and not, before the words
# are taken as the answer.
NUDGES = 2

# How many times a prompt's work is checked before it may end. A model that
# keeps finding something to do has done what it is going to, and one that
# changed nothing when it was checked is not asked a second time.
REVIEWS = 2

# How much a model may think before its next step, in characters, once a
# prompt that asked for work is under way. Most steps take a few thousand.
# gpt-oss at high effort once wrote 46,000, two and a half minutes, before
# reading a file it had read already. Asked again at once, it gets on with it.
THINK_MOST = 10000

# How many past exchanges are replayed to the expert, before the context is
# consulted. Bounded deliberately: a swapped-in expert reads the whole history
# again from cold, so an unbounded transcript would make every turn slower than
# the last. A ceiling on work, not on meaning -- fitting it to the context is
# what decides what is actually sent.
HISTORY_TURNS = 12

# The share of the context a conversation may take. The rest is for the
# answer: a conversation trimmed to exactly the context leaves the model no room
# to reply, which looks like a model refusing to answer rather than like a
# context that is full.
PROMPT_SHARE = 0.75

# How long the status line's account of a tool may be, in bytes, before it is
# cut and ends in an ellipsis.
DOING_BYTES = 64

# How a reply is drawn. A model that knows math is typeset writes $\frac{a}{b}$
# rather than a/b squeezed into a sentence. After the configured prompt rather
# than in it: that one is the person's to edit, and a saved config keeps the
# old default.
MARKDOWN = ("\n\nReplies are shown as Markdown. Write math as LaTeX: $...$ in a line, "
            "$$...$$ on its own lines.")

NO_MODEL = "No expert has a model. Add one in Settings, Experts."

# The verbs that act on the project. SEARCH has a path of its own below, and so
# do HANDOFF, which hands the next step to another expert, and DONE, which is
# not offered but is a way of saying the work is finished all the same.
PROJECT_VERBS = frozenset((
    "list", "read", "write", "edit", "find", "render", "tool", "run", "note", "fetch", "git",
    "gh", "python", "start", "stop", "logs", "screenshot", "click", "move", "type", "key",
    "scroll"))

# A call made again with nothing changed since, the second time in a prompt.
ALREADY_DONE = ("You keep making calls you have already made, with nothing changed since, and "
                "their results are above. Stop calling tools, and answer now from what you "
                "have: say what you did, and what is left.")

# The actions a prompt may take are spent, and the round asked for another.
NOT_RUN = ("That was not run: this request has used all the actions it may take. Answer now "
           "from what you have: say what you did, and what is left.")

# A run of actions going round the same few steps with nothing changing.
CYCLING = ("The last several actions changed nothing and went round the same few steps. "
           "Take a different approach. If you are stuck, stop calling tools and say what is "
           "in the way.")

DECLINED = ("The user declined that edit; the file is unchanged. Do not try the same write "
            "again. Either explain what you were going to change and why, or propose "
            "something different.")

LAST_ACTION = ("\n\nThat was the last action this request may take. Write the answer now from "
               "what you have: what you did, and what is left.")

LAST_SEARCH = ("\n\nThis was the last search available. Answer now from what you have, and say "
               "plainly if it is not enough.")

NO_SEARCH = "There is no web search here. Carry on without it."

# The checkpoint's question, out of sight: the next step, for the delegator.
NEXT_STEP = ("Before you go on: what is the next step towards what was asked? Say it in one "
             "line, as\n\nHANDOFF: <the next step>\n\nand nothing else. If it is all done, "
             "reply DONE: instead.")

# The same, of a model that calls tools in its own format.
NEXT_STEP_NATIVE = ("Before you go on: what is the next step towards what was asked? Call hand_off "
                    "with it as next_step. If it is all done, call hand_off with next_step DONE.")

# A call that could not be read as one of the tools.
BAD_CALL = "That call could not be run: {}. Check the tool's parameters and try again."

# Said to the expert seated for a step another one handed on.
TAKES_OVER = ("\n\nYou are taking over this request from {} for the next step: {}. The "
              "conversation so far is above. Carry on from it.")

# Said to an expert that handed off, when nobody fits the step better.
CARRY_ON = "\n\nNobody else fits the next step better than you. Carry on with it: {}"

# Said to an expert whose prompt was sent to it by name.
PINNED_ON = "\n\nThis request was sent to you by name, so it stays with you. Carry on."

# An answer straight after an action that failed. LFM2.5, refused a write to
# "/index.html", answered that it had made the page, with nothing written.
FAILED_LAST = ("That last action failed: {}. If what was asked still needs it, fix it and carry "
               "on. Otherwise answer, and say what failed.")

# A write or an edit in a reply the length limit cut off: what it would have
# written is not all there, and a file is never left half written.
CUT_OFF = ("That reply was cut off by the length limit before the end of what you were "
           "writing, so nothing was written. Write it again, shorter, or in parts: WRITE the "
           "first part, then EDIT to add the rest.")

# Asked before a prompt that wanted something made or changed may end.
REVIEW = ("Before you finish, check your work against what was asked: {}\n\nIf any of it is not "
          "actually done yet, a file not written or a change only described in your reply, do it now "
          "with your tools. If all of it is done, reply DONE and nothing else.")

# A round that ended with nothing said and nothing called.
SAID_NOTHING = ("Your last reply was empty. If there is more to do, do it now with your tools. "
                "Otherwise tell the person what you did.")

# A reply that went round in a loop and was stopped.
REPEATED_ITSELF = ("\n\nYour last reply started repeating itself and was stopped. Write it "
                   "again, plainly and once.")

TEXT_ONLY = "\n(This model reads text only and cannot see the picture itself.)"


# --- what is said, and how it reads ---------------------------------------------


def paragraphs(first, second):
    """Two stretches of a reply, a paragraph apart -- or whichever one there is."""
    if not first:
        return second
    if not second:
        return first
    return first + "\n\n" + second


# A line break written as HTML, which a model sometimes puts on the line before
# a call, with nothing else: not prose, and shown as the tag itself.
HTML_BREAKS = re.compile(r"(?:\s*<br\s*/?>)+\s*", re.IGNORECASE)


def prose_before_tool_call(answer, kind):
    """What the expert said before it reached for a tool.

    A reply that ends in `WRITE: src/calc.py` and a fenced block is two things:
    a sentence for the reader and a line for the machine. The line has to come
    off the transcript -- shown as an answer it reads as the expert talking
    about the protocol -- and the sentence has to stay, because it is the only
    explanation of the change that will ever be written.

    The first line that opens with the verb, searched line by line rather than
    with a bare find, so a mention of "WRITE:" inside the prose does not cut
    off the very sentence being kept. Nothing at all when there is nothing but
    the call.
    """
    verb = kind.upper() + ":"
    at = 0
    while at <= len(answer):
        start = at
        while start < len(answer) and answer[start] in " \t":
            start += 1
        if answer.startswith(verb, start):
            prose = answer[:at].rstrip("\n ")
            return "" if HTML_BREAKS.fullmatch(prose) else prose
        end = answer.find("\n", at)
        if end < 0:
            break
        at = end + 1
    return ""


def doing(kind, argument):
    """What the status line says while a tool runs: the verb and what it is
    pointed at, which is the useful half. Not "<expert> is <verb>ing": the
    verbs are nouns as much as verbs here, and half of them came out as
    "noteing" or "runing".

    Cut at DOING_BYTES and given an ellipsis, never through a character.
    """
    text = kind + " " + argument
    raw = text.encode("utf-8")
    if len(raw) <= DOING_BYTES:
        return text
    cut = DOING_BYTES
    while cut > 0 and (raw[cut] & 0xC0) == 0x80:
        cut -= 1
    return raw[:cut].decode("utf-8") + "…"


def routed_prompt(prompt, pinned, attachments):
    """What the delegator routes: the prompt, and the names of what came with it.

    What was attached says something about who should answer: "what does this
    do" means one thing beside main.rs and another beside a lease agreement.
    The names are enough to say it. A pinned prompt is not routed at all.
    """
    if pinned or not attachments:
        return prompt
    return prompt + "\n\nAttached:" + "".join(" " + name for name in attachments)


def system_prompt(params, seat, tools=True):
    """The expert's instructions for this turn, from the person's own prompt
    and what the seated model can do. Without `tools`, the same with none
    offered: the round that answers from what the turn found."""
    text = params.get("system_prompt", "")
    effort = params.get("effort", "")
    if effort and not seat.get("remote") and seat.get("takes_effort") and not seat.get("native"):
        # Where a local reasoning model looks for it. A provider takes it as a
        # field of the request instead, which the core sends with each round.
        text += "\n\nReasoning: " + effort
    text += MARKDOWN
    if not tools:
        # A model told it has tools reaches for them: a fine-tuned 3B asked
        # who wrote Hamlet called them until the turn ran out.
        return text
    if params.get("web_search"):
        # Only when the tool is switched on. An expert told it can search when
        # it cannot will offer to, which is worse than not having the tool.
        text += params.get("search_tools", "")
    # Every tool the folder allows, in every turn. There used to be a line
    # here: a cook could change the project and a chat could only talk about
    # it. That was never a distinction anybody asked for -- "fix the typo in
    # README" is a sentence, and the answer to it is the edit. The permission
    # is the folder, answered once, in the terms it is about: with none
    # trusted, there are no tools to list.
    return text + seat.get("tools", "")


# What makes a prompt a request for work rather than a question. Only ever a
# reason to ask the expert once more whether it is finished, so a wrong guess
# costs a short round and no more.
WORK_WORDS = re.compile(r"\b(add|make|create|write|build|fix|change|update|edit|implement|remove|delete|"
                        r"rename|refactor|replace|move|convert|generate|install|put|center|style|improve|"
                        r"redo|rewrite)\b", re.IGNORECASE)
JUST_ASKING = re.compile(r"\s*(what|why|how|when|where|who|which|does|do|did|is|are|was|were|explain|"
                         r"describe|summarize|compare|tell me|show me)\b", re.IGNORECASE)


def asks_for_work(prompt):
    """Whether `prompt` asks for something to be made or changed."""
    return bool(WORK_WORDS.search(prompt or "")) and not JUST_ASKING.match(prompt or "")


def actions_for(params):
    """How many actions a prompt may take: Settings, Tools, or ACTIONS."""
    try:
        return max(1, int(params.get("actions") or ACTIONS))
    except (TypeError, ValueError):
        return ACTIONS


def running_account(done):
    """What the prompt has done so far, for the top of the window: the loop's
    memory of itself, once the actions it took are older than the window."""
    if not done:
        return ""
    text = "What has been done for this request so far, oldest first:\n"
    for step in done[-ACCOUNT_LINES:]:
        text += "- " + step["summary"] + ("" if step.get("ok", True) else "  (failed)") + "\n"
    seen, files = set(), []
    for step in done:
        for path in step.get("changed") or []:
            if path not in seen:
                seen.add(path)
                files.append(path)
    if files:
        text += "Files changed: " + ", ".join(files) + "\n"
    return text


def window(log):
    """Where the turn's own messages would start if the older ones went, and
    those that stay: the last RECENT of them, from an answer of the expert's."""
    if len(log) <= RECENT:
        return 0, log
    start = len(log) - RECENT
    if log[start]["role"] != "assistant":
        start += 1
    return start, log[start:]


# What each tool did, for a model that does not read tool calls.
DID = {"read_file": "read", "write_file": "wrote", "edit_file": "edited", "list_files": "listed",
       "find_in_files": "searched for", "render_page": "drew", "run_command": "ran",
       "run_python": "ran some Python", "fetch_url": "fetched", "web_search": "searched the web for",
       "hand_off": "handed on the next step,"}


# A next step that says there is none, the way a model made to name one puts it.
NOTHING_LEFT = re.compile(
    r"^\W*(?:done|none|nothing|n/?a|no (?:further|more|next|other)\b|all (?:done|set|complete|finished)\b"
    r"|(?:the )?tasks? (?:is |are |has been |have been )?(?:complete|completed|done|finished)\b"
    r"|(?:completed|finished)\b)", re.I)
ALREADY_MADE = re.compile(r"\b(?:ha(?:s|ve) been|(?:is|are) (?:now )?(?:complete|completed|done|finished|ready|in place))\b",
                          re.I)
JUST_DONE = re.compile(r"\W*(?:done|none|nothing|n/?a)\W*", re.I)
# The word the last check asks for, left at the end of a sentence by a model
# that went on to answer properly. Only in capitals and after a full stop, so
# that "set its status to DONE" is left as it is.
TRAILING_DONE = re.compile(r"(?:(?<=[.!?:])|(?<=\n))\s*DONE[.!]?\s*$")


def nothing_left(step):
    """Whether the step a checkpoint got back is that there is none. Sent on
    as a step, "the page has been added" brought the last expert back to work
    on a finished job, and it broke the page. A real step read this way only
    stays with the expert that has it."""
    return bool(NOTHING_LEFT.match(step or "") or ALREADY_MADE.search(step or ""))


def as_done(function):
    """A tool call in words, as something done. Written as the call it was,
    with its arguments, LFM2.5 copied it: "(called edit_file: {...})" became
    the end of its answer to the person, and nothing was edited."""
    try:
        given = json.loads(function.get("arguments") or "{}")
    except ValueError:
        given = {}
    given = given if isinstance(given, dict) else {}
    what = next((str(given[key]) for key in ("path", "page", "command", "next_step", "text", "url", "query")
                 if given.get(key)), "")
    name = function.get("name") or "a tool"
    words = DID.get(name, "used " + name.replace("_", " "))
    return steps.line("I %s %s." % (words, what) if what else "I %s." % words, 200)


def plain(log):
    """The turn's messages for a model that does not read tool calls: a call
    as a line of what it did, and what came back as the next thing said."""
    out = []
    for message in log:
        if message.get("role") == "tool":
            out.append({key: value for key, value in message.items() if key != "tool_call_id"})
            out[-1]["role"] = "user"
        elif message.get("tool_calls"):
            out.append({"role": "assistant", "content": paragraphs(
                message.get("content") or "", as_done(message["tool_calls"][0]["function"]))})
        else:
            out.append(message)
    return out


def dropped_note(dropped):
    """The transcript's line for exchanges left out to fit the context.
    `dropped` counts messages, which go in pairs."""
    return "dropped %d %s to stay inside the context" % (
        dropped // 2, "earlier exchange" if dropped == 2 else "earlier exchanges")


def unanswered(shown, parsed, hit_limit, spent=True):
    """Why a turn that ended has no answer to show, or "" when it has one.

    Three ways to get here, and they want different things said. A reasoning
    model can spend its whole token budget in the channel the person does not
    see and never reach the one they do; an expert with the search tool can
    spend its last round asking to search again rather than answering; and a
    tool call can be left where the answer should be, when the last round the
    turn had was spent asking for one more action. A raw "WRITE: src/main.cpp"
    or "SEARCH: ..." is the one thing that must never be shown as an answer --
    it reads as the expert having said something. Naming the wrong reason
    sends the reader to the wrong setting.
    """
    call = parsed.get("call")
    asked_to_act = bool(call) and call.get("kind") in PROJECT_VERBS
    asked_to_search = bool(parsed.get("search"))
    if parsed.get("attempted") == "call":
        return "wrote a tool call that could not be read"
    if shown and not asked_to_act and not asked_to_search:
        return ""
    if asked_to_act and not spent:
        return "kept trying the same thing and was stopped"
    if asked_to_act:
        return ('used all the actions this request may take. Ask again to carry on, or raise '
                '"Actions per prompt" in Settings, Tools')
    if asked_to_search:
        return 'kept searching instead of answering. Raise "Searches per prompt" in Settings, Tools'
    if hit_limit:
        return ('thought until "Longest reply" ran out. Raise it in Settings, Generation, or '
                'lower the effort')
    return "stopped without answering"


def trace(**fields):
    """One line about a round, to the file $CRUCIBLE_TRACE names, for reading
    afterwards what a model really wrote. Nothing without it."""
    path = os.environ.get("CRUCIBLE_TRACE")
    if not path:
        return
    try:
        with open(path, "a", encoding="utf-8") as out:
            out.write(json.dumps(dict(fields, at=round(time.time(), 2)), ensure_ascii=False) + "\n")
    except OSError:
        pass


# --- the turn ---------------------------------------------------------------------


class Turn:
    """One turn, from its route to its end. Every call it makes of the core
    names the turn, which the core opened before asking for it."""

    def __init__(self, core, params):
        self.core = core
        self.params = params
        self.index = params.get("turn", 0)
        self.roster = routing.Roster(params.get("roster"))
        self.decision = None   # who is seated, as the delegator decided it
        self.expert = ""
        self.name = ""
        self.seat = {}
        self.size = 0          # the seated expert's context, in tokens
        self.before_call = None   # what this round said before a call it spelled out
        # A note for the next round alone, put after the instructions: who is
        # taking over, or that a reply was stopped for repeating itself.
        self.aside = ""

    # --- the core's calls -----------------------------------------------------

    def call(self, method, params=None):
        asked = dict(params or {})
        asked["turn"] = self.index
        return self.core.call(method, asked) or {}

    def mood(self, mood, text=""):
        self.core.call("mood", {"mood": mood, "text": text})

    def flags(self):
        return self.core.call("engine.flags") or {}

    def show(self, actions=(), reply=None):
        """Put what the turn did above its reply, and the reply as it now
        reads, in that order."""
        asked = {"actions": list(actions)}
        if reply is not None:
            asked["reply"] = reply
        self.call("turn.show", asked)

    def end(self, state, reason="", mood="idle", status="", actions=(), reply=None,
            remember=False, unlink=True):
        """Close the turn: finished, failed with `reason`, or canceled. The
        history first, when the exchange is worth remembering; then what is
        shown; then the turn's record; then the status line."""
        asked = {"state": state, "reason": reason, "mood": mood, "status": status,
                 "actions": list(actions), "remember": remember, "unlink": unlink}
        if reply is not None:
            asked["reply"] = reply
        self.call("turn.end", asked)

    # --- the whole of it ------------------------------------------------------

    def run(self):
        params = self.params
        pinned = params.get("pinned") or ""

        # --- who answers -------------------------------------------------------
        try:
            decision = routing.route(self.core, {
                "prompt": routed_prompt(params.get("prompt", ""), pinned,
                                        params.get("attachments") or []),
                "pinned": pinned,
                "roster": params.get("roster"),
                "seats": params.get("seats"),
                "routing": params.get("routing"),
            })
        except Disconnected:
            raise
        except Exception as error:   # the delegator's calls failed outright
            reason = "routing failed: %s" % error
            self.end("failed", reason=reason, mood="error", status=reason, unlink=False)
            return
        routed = self.call("turn.route", {"decision": decision.to_json()})
        if self.flags().get("cancel"):
            self.end("failed", reason="canceled before routing finished")
            return
        if not routed.get("has_model"):
            self.end("failed", reason=NO_MODEL, mood="error", status="no experts configured")
            return

        # --- the expert, in its seat ---------------------------------------------
        seat = self.call("turn.load")
        first_error = seat.get("error", "") if not seat.get("ok") else ""
        tried = [decision.expert]
        # An expert that cannot be seated right now is no reason to leave the
        # prompt unanswered while another one can take it. flint, 36 GB on
        # three cards, fits or does not by what the desktop has open. A prompt
        # sent to one expert by name stays with it, and says why it cannot.
        while not seat.get("ok") and seat.get("error") != "stopped" and not pinned:
            other = self.next_best(tried)
            if other is None:
                break
            routed = self.call("turn.route", {"decision": other.to_json()})
            if not routed.get("has_model"):
                tried.append(other.expert)
                continue
            self.show(actions=[{"summary": "%s could not load, so %s takes this" % (
                self.roster.label(tried[-1]), self.roster.label(other.expert)),
                "body": seat.get("error", "")}])
            self.mood("loading", self.roster.label(other.expert) + " is taking this")
            tried.append(other.expert)
            decision = other
            seat = self.call("turn.load")
        if not seat.get("ok"):
            error = seat.get("error", "")
            if error == "stopped":
                # A load the person stopped is not an error to be explained,
                # and the turn it belonged to reads as canceled, not failed.
                self.end("canceled")
            else:
                # The first expert's reason: it is the one who was asked for.
                error = first_error or error
                self.end("failed", reason=error, mood="error", status=error)
            return
        self.decision = decision
        self.expert = decision.expert
        self.name = self.roster.label(decision.expert)
        self.seat = seat
        self.mood("thinking", self.name + " is reading")

        # --- the conversation ----------------------------------------------------
        #
        # What was attached goes above what was typed, so the question comes
        # after the material it is about, the order every model reads best.
        # The core reads it for the seated model: how much of a long document
        # fits is a question about this expert's context.
        if params.get("attachments"):
            self.mood("thinking", self.name + " is reading the attachments")
        refused = self.open()
        if refused:
            self.end("failed", reason=refused, mood="error", status="context full")
            return
        self.rounds()

    def next_best(self, tried):
        """Who the delegator would seat for this prompt, of the experts with a
        model who have not been tried. None when nobody is left."""
        params = self.params
        seats = params.get("seats") or {}
        left = [one for one in params.get("roster") or []
                if one.get("id") not in tried and (seats.get(one.get("id")) or {}).get("model")]
        if not left:
            return None
        if len(left) == 1:
            return routing.Decision(left[0]["id"], 1.0, routing.FALLBACK,
                                    self.roster.label(tried[0]) + " could not load")
        try:
            decision = routing.route(self.core, {
                "prompt": routed_prompt(params.get("prompt", ""), "", params.get("attachments") or []),
                "pinned": "",
                "roster": left,
                "seats": {one["id"]: seats.get(one["id"]) for one in left},
                "routing": params.get("routing"),
            })
        except Disconnected:
            raise
        except Exception as error:   # the delegator could not be had: the first of them, then
            print("chat: could not route past %s (%s)" % (tried[-1], error), file=sys.stderr)
            return routing.Decision(left[0]["id"], 1.0, routing.FALLBACK,
                                    self.roster.label(tried[0]) + " could not load")
        return decision if decision.expert in [one["id"] for one in left] else None

    def open(self):
        """The conversation, opened for the seated expert and fitted to its
        context. What was dropped to fit is said; "" when it opened, or why it
        is refused when the person said to stop rather than drop."""
        params = self.params
        opened = self.call("turn.open", {"system": system_prompt(params, self.seat),
                                         "history": HISTORY_TURNS * 2, "share": PROMPT_SHARE})
        # Does it still fit? Asked once the expert is seated, because only the
        # expert can answer it: the context is the one it was loaded with and
        # the count is its own tokenizer's.
        size = int(opened.get("size") or 0)
        self.size = size
        used = int(opened.get("used") or 0)
        budget = int(size * PROMPT_SHARE)
        policy = params.get("overflow", "")
        if policy == "stop" and used > budget:
            # Refused rather than shortened. The model cannot tell you what it
            # stopped being able to see, so the program says it instead.
            return ('this conversation no longer fits: %d of %d tokens. Raise "Context" in '
                    'Settings, Generation, or start a new chat.' % (used, size))
        dropped = int(self.call("turn.fit", {"policy": policy, "budget": budget}).get("dropped") or 0)
        if dropped > 0:
            self.show(actions=[{"summary": dropped_note(dropped)}])
        return ""

    def could_switch(self, explicit):
        """Whether the next step may go to another expert: never for a prompt
        sent to one by name, never with nobody else to go to, and at a
        checkpoint only with a delegator to choose (keywords read a step's
        line poorly; a handoff the expert asked for may use them)."""
        if self.params.get("pinned"):
            return False
        seats = self.params.get("seats") or {}
        if sum(1 for one in seats.values() if (one or {}).get("model")) < 2:
            return False
        return explicit or (self.decision is not None and self.decision.source == routing.MODEL)

    def hand_off(self, work, explicit):
        """Route the next step, `work`, and seat the expert that wins when it
        is somebody else: at a checkpoint only when the delegator is sure of
        it. The new expert's name, or "" when the step stays."""
        params = self.params
        try:
            decision = routing.route(self.core, {
                "prompt": work,
                "pinned": "",
                "roster": params.get("roster"),
                "seats": params.get("seats"),
                "routing": params.get("routing"),
                # Worded as a task is, so routed on the seats' descriptions.
                "tasks": True,
            })
        except Disconnected:
            raise
        except Exception as error:   # the delegator could not be had; the step stays
            print("chat: the next step could not be routed (%s)" % error, file=sys.stderr)
            return ""
        trace(kind="route", step=work, expert=decision.expert, confidence=decision.confidence,
              source=decision.source, explicit=explicit)
        if (not decision.expert or decision.expert == self.expert
                or decision.source == routing.FALLBACK
                or (decision.source == routing.KEYWORD and not explicit)
                or (not explicit and decision.confidence < SWITCH_AT)):
            return ""
        return self.reseat(decision, work)

    def reseat(self, decision, work):
        """Seat the expert `decision` names for the next step, `work`, with the
        conversation as it is. One that cannot be seated leaves the step
        where it was, and says so."""
        before = (self.decision, self.expert, self.name, self.seat)
        name = self.roster.label(decision.expert)
        self.mood("loading", name + " is taking over")
        routed = self.call("turn.route", {"decision": decision.to_json()})
        seat = self.call("turn.load") if routed.get("has_model") else {"ok": False, "error": "no model"}
        why = "" if seat.get("ok") else seat.get("error", "") or "could not load"
        if not why:
            self.decision, self.expert, self.name, self.seat = decision, decision.expert, name, seat
            why = self.open()
        if why:
            # Back to the one that had it: its model is still held, so this
            # costs a route and no load.
            self.decision, self.expert, self.name, self.seat = before
            self.call("turn.route", {"decision": before[0].to_json()})
            if not self.call("turn.load").get("ok") or self.open():
                raise RuntimeError("%s could not take over (%s), and %s could not go on"
                                   % (name, why, before[2]))
            self.show(actions=[{"summary": "%s could not take over: %s" % (name, why)}])
            return ""
        self.show(actions=[{"summary": "%s takes over: %s" % (name, steps.line(work, 80))}])
        self.mood("thinking", name + " is reading")
        return name

    def native(self):
        """Whether the seated expert calls tools in its own format."""
        return bool(self.seat.get("native"))

    def for_seat(self, log):
        """The turn's messages as the seated expert reads them. An expert that
        took over from one with another way of calling tools reads them too."""
        return list(log) if self.native() else plain(log)

    @staticmethod
    def said_call(log, answer, raw):
        """Keep what the expert said, with the call it made in its own format."""
        message = {"role": "assistant", "content": answer}
        if raw:
            message["tool_calls"] = [{"id": raw.get("id") or "call_1", "type": "function",
                                      "function": {"name": raw.get("name", ""),
                                                   "arguments": raw.get("arguments") or "{}"}}]
        log.append(message)

    @staticmethod
    def answered(log, raw, text, **more):
        """Keep what came back: the call's result when it made one, otherwise
        the next thing said to it."""
        message = ({"role": "tool", "tool_call_id": raw.get("id") or "call_1", "content": text} if raw
                   else {"role": "user", "content": text})
        message.update(more)
        log.append(message)

    def prose(self, answer, kind, raw):
        """What a round said beside its call. A call in the model's own format
        is not in the words at all, or was spelled out after them."""
        # What gpt-oss writes beside a call is its working out loud ("we also
        # need to... let's list the files again"), and its answer comes last.
        if raw and self.seat.get("takes_effort"):
            return ""
        said = (self.before_call if self.before_call is not None else
                answer.strip() if raw else prose_before_tool_call(answer, kind))
        # gpt-oss sometimes leaves a scrap of its own format beside a call, one
        # word and no capital: a reply began "comment?".
        return "" if said and len(said.split()) < 2 and said == said.lower() else said

    def crowded(self, used):
        """Whether a prompt of `used` tokens is near what the context holds."""
        return self.size > 0 and used > self.size * PROMPT_SHARE * CROWDED

    def checkpoint(self, shown, note):
        """Ask the seated expert, out of sight, what the next step is, and let
        the delegator choose who takes it. `shown` is what it is shown of the
        turn so far, ending in the result of its last action, and `note` what
        was done before that. The step, when another expert took it, or "".

        Asked under the same instructions as every other round, with the
        question at the very end. Asked under others, the model read the whole
        conversation again to answer it, and once more for the round after.
        """
        if not shown or shown[-1]["role"] not in ("user", "tool"):
            return ""
        messages = [dict(m) for m in self.for_seat(shown)]
        messages[-1]["content"] = (messages[-1]["content"] + "\n\n"
                                   + (NEXT_STEP_NATIVE if self.native() else NEXT_STEP))
        self.mood("thinking", self.name + " is choosing the next step")
        said = self.call("turn.round", {"messages": messages, "note": note, "paragraph": False,
                                        "quiet": True, "quick": True, "most": CHECKPOINT_TOKENS,
                                        "native": self.native(),
                                        "system": system_prompt(self.params, self.seat)})
        trace(kind="checkpoint", expert=self.expert, answer=said.get("answer", ""),
              reasoning=len(said.get("reasoning", "")), tokens=said.get("output_tokens"),
              prompt=said.get("prompt_tokens"), reused=said.get("prompt_reused"),
              prompt_ms=said.get("prompt_ms"), output_ms=said.get("output_ms"))
        if said.get("error") or said.get("canceled") or said.get("looped"):
            return ""
        raw = (said.get("calls") or [None])[0]
        parsed = (self.core.call("tools.native", {"name": raw.get("name", ""),
                                                  "arguments": raw.get("arguments", "")}) if raw else
                  self.core.call("tools.parse", {"answer": said.get("answer", ""),
                                                 "reasoning": said.get("reasoning", "")})) or {}
        call = parsed.get("call") or {}
        work = (call.get("argument") or "").strip() if call.get("kind") == "handoff" else ""
        trace(kind="next_step", expert=self.expert, step=work, called=(raw or {}).get("name", ""))
        if nothing_left(work):
            return ""
        return work if work and self.hand_off(work, explicit=False) else ""

    def rounds(self):
        """Ask, and act on what the expert reached for, until it answers."""
        params = self.params
        workshop = bool(self.seat.get("workshop"))
        searching = bool(params.get("web_search"))
        limit = actions_for(params)
        searches = int(params.get("search_rounds") or 0)
        root = params.get("root", "")
        log = []               # this turn's own messages, after the conversation it opened with
        start = 0              # where in `log` what the expert is shown begins
        note = ""              # the account of what was done before that
        used = 0               # how long the last round's prompt was, in tokens
        done = []              # what each action did, for the account
        earlier = ""           # what the rounds before this one left on screen
        repeats = steps.Repeats(root)
        cycles = steps.Cycles()
        searched = []
        actions = since_checkpoint = nudges = loops = cycle_strikes = reviews = 0
        reviewing = False       # this round is the check before the prompt ends
        rushing = False         # this round follows one that thought for too long
        edits = reviewed_at = 0 # files changed so far, and when it was last checked
        before_check = ""       # what was on screen before the answer being checked
        working = workshop and asks_for_work(params.get("prompt"))
        declined = False        # the person said no to an edit, and is not to be pressed
        since_switch = SETTLE   # actions since another expert took over
        touched = set()         # the files this prompt has changed
        failed = ""            # what the last action said when it failed, until it is answered
        canceled = hit_limit = False
        output_tokens = 0
        shown = ""
        # The round that answers from what the turn found, with no tools
        # offered: the one after the last action the prompt may take, or after
        # it went round once too often. Either way another round with tools on
        # offer is another call.
        final = False
        # Rounds of any kind, so nudges and handoffs cannot go on for ever.
        most = limit * 2 + 20

        for _ in range(most):
            # What it is shown only grows until the context is nearly full,
            # and then the older exchanges go together.
            if self.crowded(used) and window(log)[0] > start:
                start = window(log)[0]
                note = running_account(done)
            shown_log = self.for_seat(log[start:])
            system = system_prompt(params, self.seat, tools=not final)
            if self.aside:
                system += self.aside
                self.aside = ""
            # A turn that reads a file, then writes one, then answers says
            # something each time, and each is kept: a paragraph apart, rather
            # than run into the next sentence or replaced by it.
            said = self.call("turn.round", {"messages": shown_log, "note": note,
                                            "paragraph": bool(earlier), "system": system,
                                            "quiet": reviewing, "native": self.native(),
                                            "tools": not final, "rush": rushing,
                                            "think_most": THINK_MOST if working and actions else 0})
            used = int(said.get("prompt_tokens") or 0)
            if said.get("no_native") and self.native():
                # A template that will not take this conversation: the lines
                # every model can be told to write, for the rest of the turn.
                self.seat = dict(self.seat, native=False, tools=self.seat.get("text_tools", ""))
                continue
            if said.get("too_long"):
                # Longer than the context holds. The older exchanges go, and
                # with nothing older to let go there is nothing to ask.
                if window(log)[0] > start:
                    start = window(log)[0]
                    note = running_account(done)
                    continue
                reason = "this request no longer fits the context: %d tokens" % int(said["too_long"])
                self.end("failed", reason=reason, mood="error", status="context full")
                return
            answer = said.get("answer", "")
            reasoning = said.get("reasoning", "")
            trace(kind="round", expert=self.expert, answer=answer, reasoning=len(reasoning),
                  tokens=said.get("output_tokens"), hit_limit=said.get("hit_limit"),
                  looped=said.get("looped"), overthought=said.get("overthought"), rushed=rushing,
                  error=said.get("error"), prompt=said.get("prompt_tokens"), reused=said.get("prompt_reused"),
                  prompt_ms=said.get("prompt_ms"), output_ms=said.get("output_ms"))
            # That a different model answered than the one in the seat, mostly.
            notes = [{"summary": note} for note in said.get("notes") or []]
            if said.get("error"):
                # The provider could not be asked, or stopped partway, or the
                # model declined. Only a provider's model gets here: a local
                # one that is loaded answers.
                error = said["error"]
                reply = None
                if said.get("discard"):
                    reply = earlier   # what streamed before a refusal is not half an answer
                    if earlier:
                        # A reply that is not empty is kept in place of the
                        # reason, which would then go unsaid.
                        notes.append({"summary": error})
                elif answer:
                    # A reply that had started is kept, but then nothing would
                    # say why it stops where it does.
                    notes.append({"summary": "cut short: " + error})
                self.end("failed", reason=error, mood="error", status=error, actions=notes, reply=reply)
                return
            if notes:
                self.show(actions=notes)
            canceled = bool(said.get("canceled"))
            hit_limit = bool(said.get("hit_limit"))
            output_tokens += int(said.get("output_tokens") or 0)
            shown = said.get("reply", "")
            if canceled:
                break
            if said.get("looped"):
                # What it wrote went round and was stopped: off the screen, and
                # asked again once, plainly. Twice is a model that cannot.
                self.show(reply=earlier)
                shown = earlier
                loops += 1
                if loops > 1:
                    break
                self.aside = REPEATED_ITSELF
                continue
            if said.get("overthought") and not rushing:
                # It went on thinking past anything a next step needs. Asked
                # again at once, with the thinking cut short, so that it acts.
                self.show(reply=earlier)
                shown = earlier
                rushing = True
                continue
            rushing = False
            if final or (not workshop and not searching):
                break

            # The call it made in its own format, and otherwise a line in its
            # words: a model offered functions may still write one.
            raw = (said.get("calls") or [None])[0] if workshop else None
            if raw:
                parsed = self.core.call("tools.native", {"name": raw.get("name", ""),
                                                         "arguments": raw.get("arguments", "")}) or {}
                if parsed.get("error"):
                    earlier = paragraphs(earlier, answer.strip())
                    self.show(reply=earlier)
                    shown = earlier
                    self.said_call(log, answer, raw)
                    self.answered(log, raw, BAD_CALL.format(parsed["error"]))
                    nudges += 1
                    if nudges > NUDGES:
                        break
                    continue
            else:
                parsed = self.core.call("tools.parse", {"answer": answer, "reasoning": reasoning}) or {}
            self.before_call = parsed.get("prose")
            call = (parsed.get("call") or {}) if workshop else {}
            kind = call.get("kind", "")

            # --- the next step, to somebody else ----------------------------------
            if kind == "handoff" and NOTHING_LEFT.match(call.get("argument") or ""):
                # Handing on "it is all done" is finishing.
                said_done = call.get("argument") or ""
                call = dict(call, kind="done", argument="" if JUST_DONE.fullmatch(said_done) else said_done)
                kind = "done"
            if kind == "handoff":
                work = (call.get("argument") or "").strip()
                earlier = paragraphs(earlier, self.prose(answer, kind, raw))
                self.show(reply=earlier)
                shown = earlier
                self.said_call(log, answer, raw)
                if not self.could_switch(explicit=True):
                    self.answered(log, raw, (PINNED_ON if params.get("pinned") else
                                             CARRY_ON.format(work)).strip())
                    continue
                before = self.name
                taken = self.hand_off(work or params.get("prompt", ""), explicit=True)
                if taken:
                    since_switch = 0
                self.answered(log, raw, (TAKES_OVER.format(before, work) if taken
                                         else CARRY_ON.format(work)).strip())
                since_checkpoint = 0
                cycles.clear()
                continue

            # --- finished, said in a word ------------------------------------------
            if kind == "done":
                # Not offered, but plain enough: the work is finished, and what
                # follows the word is what the person is told.
                said_done = (call.get("argument") or "").strip()
                shown = paragraphs(paragraphs(earlier, self.prose(answer, kind, raw)), said_done)
                self.show(reply=shown)
                break

            acting = kind in PROJECT_VERBS
            query = "" if acting else parsed.get("search") or ""
            if reviewing and not acting and not query:
                # It says it is finished, or has nothing to add: what it
                # answered before it was asked stands, and what it said to
                # the check comes off. A reply once ended "All requirements
                # have been met. DONE".
                self.show(reply=earlier)
                shown = earlier
                break
            if reviewing:
                # It found more to do, so what it said about being finished
                # was early. The answer it ends on says it instead, rather
                # than the person reading "all done" three times over.
                earlier = before_check
                self.show(reply=earlier)
            reviewing = False
            if not acting and not query:
                # An answer straight after a failed action is asked once to
                # face the failure, and goes off the screen meanwhile: it may
                # be saying the work was done when it was not.
                if workshop and failed:
                    log.append({"role": "assistant", "content": answer})
                    log.append({"role": "user", "content": FAILED_LAST.format(failed)})
                    failed = ""
                    self.show(reply=earlier)
                    continue
                # A reply that nearly called a tool and did not is told what
                # was wrong with it, a couple of times. Anything else is the
                # answer, which is the ordinary way a prompt ends.
                if workshop and parsed.get("attempted", "none") not in ("none", "", None) and nudges < NUDGES:
                    nudges += 1
                    log.append({"role": "assistant", "content": answer})
                    log.append({"role": "user", "content": steps.nudge_for(parsed)})
                    self.show(reply=earlier)
                    continue
                # Nothing said and nothing called is not an answer, and a
                # thinking model does it: asked once to say or do something.
                if (workshop and not answer.strip() and not final and not hit_limit
                        and int(said.get("output_tokens") or 0) > 0 and nudges < NUDGES):
                    nudges += 1
                    log.append({"role": "assistant", "content": answer})
                    log.append({"role": "user", "content": SAID_NOTHING})
                    continue
                # The prompt wanted something made or changed: checked once
                # more, out of sight, before it ends.
                if (workshop and not final and reviews < REVIEWS and not declined
                        and asks_for_work(params.get("prompt")) and (not reviews or edits > reviewed_at)):
                    reviews += 1
                    reviewed_at = edits
                    reviewing = True
                    before_check = earlier
                    earlier = shown
                    log.append({"role": "assistant", "content": answer})
                    log.append({"role": "user", "content": REVIEW.format(steps.line(params.get("prompt", ""), 600))})
                    continue
                break
            nudges = 0

            if query:
                if not searching:
                    self.said_call(log, answer, raw)
                    self.answered(log, raw, NO_SEARCH)
                    self.show(reply=earlier)
                    continue
                spent = len(searched) >= searches
                final = self.search(query, answer, earlier, searched, log, spent, raw) or final
                continue

            if actions >= limit:
                # Every action spent, and this round asked for another. What it
                # asked for is not run, and comes off the reply the way a call
                # that ran does. The next round answers.
                earlier = paragraphs(earlier, self.prose(answer, kind, raw))
                self.show(reply=earlier)
                self.said_call(log, answer, raw)
                self.answered(log, raw, NOT_RUN)
                final = True
                continue

            # --- a file cut off -------------------------------------------------
            if hit_limit and kind in ("write", "edit"):
                earlier = paragraphs(earlier, self.prose(answer, kind, raw))
                self.show(reply=earlier)
                self.said_call(log, answer, raw)
                self.answered(log, raw, CUT_OFF)
                continue

            # --- the same call again --------------------------------------------
            if repeats.again(call, start):
                kept = paragraphs(earlier, self.prose(answer, kind, raw))
                self.show(reply=kept)
                earlier = kept
                self.said_call(log, answer, raw)
                if repeats.strikes > 1:
                    self.answered(log, raw, ALREADY_DONE)
                    final = True
                else:
                    self.answered(log, raw, steps.REPEAT_NUDGE.format("request"))
                continue

            earlier, result = self.act(call, answer, earlier, log, actions + 1 >= limit, raw)
            actions += 1
            since_checkpoint += 1
            if result is None:
                declined = True
                done.append({"summary": "declined: " + steps.spoken(call), "ok": False})
                continue
            ok = bool(result.get("ok", True))
            failed = ("" if ok or steps.needs_a_page(call, result) else
                      steps.line(result.get("summary") or result.get("output") or steps.spoken(call), 200))
            since_switch += 1
            # A piece of work finished: a file this prompt had not changed yet.
            new_work = bool(set(result.get("changed") or []) - touched)
            touched.update(result.get("changed") or [])
            edits += 1 if result.get("changed") else 0
            done.append({"summary": result.get("summary") or steps.spoken(call),
                         "ok": ok, "changed": result.get("changed") or []})
            repeats.ran(call, result, len(log) - 1)
            if cycles.add(call, bool(result.get("changed"))):
                cycle_strikes += 1
                cycles.clear()
                if cycle_strikes > 1:
                    log[-1]["content"] += "\n\n" + ALREADY_DONE
                    final = True
                    continue
                log[-1]["content"] += "\n\n" + CYCLING

            # --- who has the next step -------------------------------------------
            due = new_work or since_checkpoint >= CHECKPOINT_EVERY
            if due and since_switch >= SETTLE and actions < limit and self.could_switch(explicit=False):
                since_checkpoint = 0
                before = self.name
                work = self.checkpoint(log[start:], note)
                if work:
                    self.aside = TAKES_OVER.format(before, work)
                    since_switch = 0
                    cycles.clear()

        # --- the end of the turn ------------------------------------------------
        #
        # Only an exchange that produced an answer is remembered, so a canceled
        # turn (or one that spent its whole budget thinking) does not poison
        # the context of the next one. Remembered before a placeholder is put
        # in its place: what goes into the history is what the model said, not
        # what Crucible said about it.
        if reviews and shown:
            finished = TRAILING_DONE.sub("", shown).rstrip()
            if finished and finished != shown:
                self.show(reply=finished)
                shown = finished
        answered = not canceled and output_tokens > 0
        why = ""
        if answered:
            parsed = (self.core.call("tools.parse", {"answer": shown, "reasoning": ""}) or {}) if shown else {}
            why = unanswered(shown, parsed, hit_limit, actions >= limit)
            if why and not workshop and (parsed.get("call") or {}).get("kind") in PROJECT_VERBS:
                why = "reached for the project with no folder open. Open one to let it work there"
            if loops > 1 and not shown:
                why = "kept repeating itself and was stopped"
        # Finished only now, with the reply it will keep: the session is saved
        # as a turn finishes, and a turn finished before its placeholder saved
        # the raw call, which came back on resume.
        self.end("finished", status="canceled" if canceled else "", remember=answered and bool(shown),
                 reply="(" + why + ")" if why else None)

    def act(self, call, answer, earlier, log, last, raw=None):
        """Run the project call `call` the expert made, put what came of it
        above the reply, and hand the result back. The reply as it now reads,
        and what the tool said, or None for an edit the person declined."""
        kind = call.get("kind", "")
        argument = call.get("argument", "")
        self.mood("thinking", doing(kind, argument))

        # The turn so far, and this round's prose up to the call.
        kept = paragraphs(earlier, self.prose(answer, kind, raw))
        changes_a_file = kind in ("write", "edit")

        # --- the gate ---------------------------------------------------------
        #
        # A write or an edit is the call that changes something the person
        # owns, so it is the one that stops and asks, unless they have said
        # not to, which is read now, so the button pressed during a turn
        # counts from the next write. Everything else either changes nothing
        # or was agreed to by trusting the folder. The core asks about an edit
        # as the file it would leave, and an edit that would not apply is not
        # asked about at all: it fails with its reason when it is run.
        if changes_a_file and not self.flags().get("auto_edits"):
            approval = self.call("turn.ask_edit", {"call": call, "reply": kept})
            if not approval.get("approved"):
                # This round described a change that did not happen; what came
                # before it still stands.
                self.show(actions=[{"summary": "declined the edit to " + argument}], reply=earlier)
                self.said_call(log, answer, raw)
                self.answered(log, raw, DECLINED)
                return earlier, None

        result = self.call("turn.tool", {"call": call})
        # The diff a write made, or the output a command printed, kept for the
        # transcript rather than only handed to the model, and the protocol
        # line off the reply, with the prose around it staying.
        self.show(actions=[{"summary": result.get("summary", ""), "body": result.get("detail", ""),
                            # The file it changed, from the project's root, which is
                            # what the shelf opens: not the path as the model wrote
                            # it, and nothing for a write that changed nothing.
                            "language": (result.get("changed") or [""])[0] if changes_a_file else "",
                            "picture": result.get("picture", "")}],
                  reply=kept)
        self.said_call(log, answer, raw)

        # A picture the tool produced goes to a model that can see one, as a
        # picture, and with every round after this one; one that cannot is
        # told so, beside whatever words were read off it. A RUN that only
        # printed lines of a file is told the READ that numbers them.
        text = result.get("output", "") + steps.read_hint(call, result, self.params.get("root", ""))
        more = {}
        if result.get("pictures"):
            if result.get("sees"):
                more["pictures"] = result["pictures"]
            else:
                text += TEXT_ONLY
        if last:
            text += LAST_ACTION
        self.answered(log, raw, text, **more)
        return kept, result

    def search(self, query, answer, earlier, searched, log, spent, raw=None):
        """Look up what the expert asked for, and hand back what was found.
        Whether the next round is the one that answers with no tools.

        Asking twice for the same thing is a model that has read the results
        and not known what to do with them. Running the search again would hand
        back the same page and invite it to do the same, so it is told instead.
        A search past the last one allowed is not run either.
        """
        repeat = query in searched or spent
        searched.append(query)
        self.mood("thinking", ('already searched for "%s"' if query in searched[:-1] else
                               'searching for "%s"') % query)

        actions = []
        if repeat:
            handback = ('You have already searched for "%s" and been given the results above.' % query
                        if query in searched[:-1] else "There are no searches left for this request.")
        else:
            found = self.call("turn.search", {"query": query})
            count = int(found.get("count") or 0)
            actions.append({"summary": 'searched "%s": ' % query + (
                "%d result%s from %s" % (count, "" if count == 1 else "s", found.get("provider", ""))
                if count else found.get("error", ""))})
            handback = found.get("text", "")
        # That round asked for something rather than answering. The next round
        # writes the answer, and the request should not be sitting above it.
        self.show(actions=actions, reply=earlier)

        # Only the answer goes back, never the reasoning: the formats that
        # produce reasoning say to drop it from the context, and feeding it
        # back teaches the model that thinking aloud is part of the transcript.
        self.said_call(log, answer, raw)
        last = len(searched) >= int(self.params.get("search_rounds") or 0)
        if repeat or last:
            # The last search it is allowed. Saying so is the difference
            # between an answer and a model that asks to search again and ends
            # the turn with nothing in it.
            handback += LAST_SEARCH
        self.answered(log, raw, handback)
        return repeat and not self.seat.get("workshop")


def turn(core, params):
    """Answer one chat turn, from its route to its end."""
    chat = Turn(core, params)
    try:
        chat.run()
    except Disconnected:
        raise
    except Exception as error:   # a call to the core that failed, or a bug here
        # The turn ends with the reason rather than being left open with
        # nobody answering it. (traceback is read in here, as in rpc.py.)
        import traceback
        traceback.print_exc(file=sys.stderr)
        try:
            chat.end("failed", reason=str(error), mood="error", status=str(error))
        except Exception:   # the core closes a turn left open itself
            pass
    return {}
