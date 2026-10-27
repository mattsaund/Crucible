# SPDX-License-Identifier: MIT
"""What a message becomes: an answer, or a build.

There is one box, and what is typed into it is decided here rather than chosen
by whoever typed it. A question, an explanation, a short piece of code or a
small change to a file is answered: a chat turn, which reads and writes the
project with the tools it has. A request to make or change software (an app, a
site, a tool of several files, a feature added to a project) is built: an
architect plans it and agents work through the plan.

Decided the way routing.py decides who answers, and kept apart from it, so a
change to how seats are chosen does not move this. The delegator is shown two
plain descriptions and seven worked messages of each kind, the two labels are
scored through the core, the lean it has on a message with no content is taken
out, and the two orders the labels can be shown in are averaged. Keywords
decide when there is no delegator, or none that can be had for this message.

The two mistakes are not the same size. A question started as a build is a
plan and minutes of agents' work nobody asked for; a build answered as a chat
turn is a turn that writes what it can and says what is left. So a message is
built only when the delegator gives a build even odds or better (BUILD_AT) and
its words ask for software too: every expensive mistake the delegator made
below was a message the keywords read as an answer.

Measured with crucible-routebench --decide on LFM2.5-1.2B, about 25 ms a
message. tools/routes/answer-or-build.json, 24 answers and 22 builds with the
near misses among them: 44/46 as decided here, every answer right and 20
builds (the delegator alone, 43: one answer and two builds wrong). The prompt
was tuned on that set, so tools/routes/answer-or-build-held-out.json was
written once it was settled: 17/20, every answer right and 7 builds (the
delegator alone, 16: two answers and two builds). The keywords alone score 46
and 18, but were written with the first set in view. What the tuning found,
on the 46: with no calibration nothing was a build (LFM leans hard to
"Answer"); four worked messages a kind gave 34, six 38, seven 43; "Software"
for the label did better with six (43) and worse with seven (38); a yes or no
question in place of the two labels gave 34; saying whether a project with
files is open moved nothing; and a wider description of an answer ("any other
writing") made both sets worse. "The build is finished" and "write me a haiku
about compilers" are builds to the delegator whatever it is shown.
"""

import math
import os
import re
import time

from .routing import answer_prefix, let_go
from .rpc import Disconnected

ANSWER = "answer"
BUILD = "build"

# Where a decision came from.
MODEL = "delegator"
KEYWORD = "keywords"

# What the delegator writes for each, which is what is scored.
LABELS = {ANSWER: "Answer", BUILD: "Build"}

# Plain words for each, the way a person would tell the two apart.
DESCRIPTIONS = {
    ANSWER: ("a question, an explanation or advice, a short piece of code written in the reply, "
             "or a small fix to one file"),
    BUILD: ("making or changing software: an app, a website, a game, a tool of several files, "
            "a project, or a new feature in one"),
}

# Worked messages, shown as turns the delegator has already answered: a small
# delegator goes by these far more than by the descriptions (see
# routing.ModelRouter.conversation). None of them is one of the measured cases,
# which a test holds them to.
EXAMPLES = {
    ANSWER: ("What does a mutex do?",
             "Write a JavaScript function that sums an array.",
             "How do I make a website responsive?",
             "Fix the spelling mistake in the title of index.html.",
             "Why is my CSS grid not centering its items?",
             "Show me how to read a JSON file in Go.",
             "Summarize the notes I attached."),
    BUILD: ("Make a weather app that shows the forecast for my city.",
            "Build a website for a dog-walking business.",
            "Create a tic-tac-toe game with a computer opponent.",
            "Add a search bar to this app that filters the list.",
            "Make this app work offline and sync when it reconnects.",
            "Build an inventory system for a small shop, with a database.",
            "Rewrite this tool in Rust and split it into modules."),
}

# The odds of a build at which a message is one.
BUILD_AT = 0.5

# How much of the delegator's lean to take out: all of it. With two labels the
# lean is the whole of what a message with no content scores, and none of it
# is about the message.
CALIBRATION = 1.0

# What a message with no content looks like; see routing.CONTENT_FREE.
CONTENT_FREE = ("N/A", "", "...")


class Decision:
    __slots__ = ("kind", "confidence", "source", "detail")

    def __init__(self, kind=ANSWER, confidence=0.0, source=KEYWORD, detail=""):
        self.kind = kind
        self.confidence = confidence
        self.source = source
        self.detail = detail

    def to_json(self):
        return {"kind": self.kind, "confidence": self.confidence, "source": self.source,
                "detail": self.detail}


def message_of(prompt, attachments=()):
    """What is decided on: the prompt, and the names of what came with it,
    which can say as much as the words do ("make this" beside a sketch)."""
    if not attachments:
        return prompt
    return prompt + "\n\nAttached:" + "".join(" " + name for name in attachments)


# --- keywords ------------------------------------------------------------------
#
# For a roster with no delegator, or one that cannot be had: rough, and leaning
# to an answer, which is the cheaper mistake. Written with the measured cases
# in view, so what they score there flatters them.

# A request for something to be made...
_MAKE = (r"(?:build|make|create|develop|code|program|write|generate|scaffold|set\s*up|"
         r"implement|design|put\s+together|spin\s+up|turn|convert|port|migrate|rewrite)")
# ...that is a piece of software rather than a few lines of it.
_THING = (r"(?:apps?|application|website|site|web\s*page|landing\s+page|game|tool|cli|"
          r"command[- ]line|api|server|back\s*end|front\s*end|extension|plug-?in|add-?on|bot|"
          r"dashboard|tracker|editor|program|project|package|library|service|interface|ui|"
          r"timer|calculator|organizer|planner|clone|platform|portfolio|blog|store|shop|"
          r"screen|page)")

MAKE_THING = re.compile(r"\b%s\b.{0,80}?\b%s\b" % (_MAKE, _THING), re.I | re.S)
WANT_THING = re.compile(r"\b(?:i\s+need|i\s+want|i'd\s+like|give\s+me)\b.{0,60}?\b%s\b" % _THING,
                        re.I | re.S)
# Software described rather than asked for, the way a directive was typed into
# Build: "a budgeting app that charts my gross and net revenue". A new thing,
# so "a" or "an": "the app crashes" is about one already there.
DESCRIBE_THING = re.compile(r"^\s*an?\b[^.?!\n]{0,60}?\b%s\b" % _THING, re.I)
# A question, by how it starts or ends.
QUESTION = re.compile(r"^\s*(?:what|why|how|when|where|who|which|whose|is|are|was|were|does|do|"
                      r"did|should|explain|describe|tell\s+me)\b|\?\s*$", re.I)
# A request put as a question: "can you build me...".
ASKED_TO = re.compile(r"^\s*(?:can|could|would|will)\s+you\b|^\s*please\b", re.I)
# A change to what is there, in a folder that has something in it.
CHANGE = re.compile(r"^\s*(?:now\s+|then\s+|also\s+)?(?:add|implement|support|port|migrate|"
                    r"rewrite|refactor|convert|turn|split|redesign)\b", re.I)
# What is too small to plan: a line, a name, a comment. "A line" and not
# "line", which is in "command-line".
SMALL = re.compile(r"\b(?:docstring|comment|typo|spelling|print|log\s+line|import|type\s+hint|"
                   r"semicolon|variable|(?:a|one|the)\s+(?:line|word)|one-liner|rename|loop|"
                   r"regex|function|method)\b", re.I)


def keyword_decide(prompt, files=False):
    """Answer or build by the words alone. `files` says the folder the
    message is about has something in it to change."""
    text = prompt or ""
    if QUESTION.search(text) and not ASKED_TO.search(text):
        return Decision(ANSWER, 0.7, KEYWORD, "a question")
    made = MAKE_THING.search(text)
    if made and not SMALL.search(made.group(0)):
        return Decision(BUILD, 0.7, KEYWORD, "asks for software to be made")
    if WANT_THING.search(text):
        return Decision(BUILD, 0.6, KEYWORD, "asks for software")
    if DESCRIBE_THING.search(text):
        return Decision(BUILD, 0.6, KEYWORD, "describes software to be made")
    if files and CHANGE.search(text) and not SMALL.search(text):
        return Decision(BUILD, 0.6, KEYWORD, "a change to the project")
    return Decision(ANSWER, 0.6, KEYWORD, "nothing to build")


# --- the delegator ---------------------------------------------------------------


class Decider:
    """Asks the delegator, through the core, whether a message is a build."""

    # The measured lean, and the prompts' shared beginnings, by the file and
    # what it was shown: see routing.ModelRouter, which keeps the same two.
    _bias = {}
    _prefix = {}

    def __init__(self, core, delegator_path, calibration=CALIBRATION, method=""):
        self.core = core
        self.path = delegator_path
        self.calibration = calibration
        methods = set((method or "").split(","))
        # Both orders, each kind shown first once: a delegator leans towards
        # where a label sits as well as towards what it says. "single", for
        # crucible-routebench, scores the first alone.
        self.orders = [(ANSWER, BUILD)] if "single" in methods else [(ANSWER, BUILD), (BUILD, ANSWER)]
        # Whether the folder having files in it is said to the delegator; off
        # with "blind", for crucible-routebench to measure what it is worth.
        self.tell_files = "blind" not in methods
        # Whether a build the words do not ask for is answered instead; off
        # with "alone", which is the delegator's decision and nothing else.
        self.guarded = "alone" not in methods

    def shown(self, order, files):
        """The instructions and the worked messages, in `order`."""
        system = ("You decide what a message to a coding assistant asks for.\n"
                  "Reply with only one word: %s or %s.\n\n" % (LABELS[order[0]], LABELS[order[1]]))
        for kind in order:
            system += "%s: %s.\n" % (LABELS[kind], DESCRIPTIONS[kind])
        if self.tell_files:
            system += ("\nThe person has a project open with files in it.\n" if files
                       else "\nNo project with files is open.\n")
        # Taking turns, so the last thing shown before the message is one kind
        # in one order and the other kind in the other.
        examples = []
        for i in range(max(len(v) for v in EXAMPLES.values())):
            for kind in order:
                if i < len(EXAMPLES[kind]):
                    examples.append((EXAMPLES[kind][i], LABELS[kind]))
        return system, tuple(examples)

    def conversation(self, message, order, files):
        system, examples = self.shown(order, files)
        messages = [{"role": "system", "content": system}]
        for example, label in examples:
            messages.append({"role": "user", "content": example})
            messages.append({"role": "assistant", "content": label})
        messages.append({"role": "user", "content": message})
        rendered = self.core.call("delegator.format", {"messages": messages})["text"]
        return rendered + answer_prefix(rendered)

    def prefix(self, order, files):
        """What every message's prompt in this order starts with, which the
        core keeps read between messages: see routing.ModelRouter.prefix."""
        key = (self.path,) + self.shown(order, files)
        if key not in Decider._prefix:
            rendered = [self.conversation(message, order, files) for message in ("a", "b")]
            Decider._prefix[key] = os.path.commonprefix(rendered)
        return Decider._prefix[key]

    def score(self, message, order, files, cancelable=True):
        """[answer, build] for one order, or None, or "canceled"."""
        reply = self.core.call("delegator.score",
                               {"prompt": self.conversation(message, order, files),
                                "labels": [LABELS[ANSWER], LABELS[BUILD]],
                                "prefix": self.prefix(order, files), "cancelable": cancelable})
        if reply.get("canceled"):
            return "canceled"
        scores = reply.get("scores") or []
        if len(scores) != 2 or any(s is None for s in scores):
            return None
        return [float(s) for s in scores]

    def bias(self, order, files):
        key = (self.path,) + self.shown(order, files)
        if key not in Decider._bias:
            total, measured = [0.0, 0.0], 0
            for message in CONTENT_FREE:
                # Not stopped by Stop: a lean half measured is a wrong one.
                scores = self.score(message, order, files, cancelable=False)
                if isinstance(scores, list):
                    total = [t + s for t, s in zip(total, scores)]
                    measured += 1
            Decider._bias[key] = [t / measured for t in total] if measured else [0.0, 0.0]
        return Decider._bias[key]

    def build_odds(self, message, files, record=None):
        """The odds the message is a build, from 0 to 1, or why there are
        none. Each order's scores and lean go on `record` when one is given."""
        total = [0.0, 0.0]
        for order in self.orders:
            bias = self.bias(order, files)
            scores = self.score(message, order, files)
            if not isinstance(scores, list):
                return scores
            if record is not None:
                record.append({"first": order[0], "scores": scores, "bias": bias})
            total = [t + s - self.calibration * b for t, s, b in zip(total, scores, bias)]
        answer, build = (t / len(self.orders) for t in total)
        return 1.0 / (1.0 + math.exp(max(-60.0, min(60.0, answer - build))))

    def decide(self, message, files=False, build_at=BUILD_AT):
        started = time.monotonic()
        odds = self.build_odds(message, files)
        elapsed = int((time.monotonic() - started) * 1000)
        if odds == "canceled":
            return Decision(ANSWER, 0.0, MODEL, "canceled")
        if odds is None:
            decision = keyword_decide(message, files)
            decision.detail = "the delegator could not be scored, used keywords"
            return decision
        if odds < build_at:
            return Decision(ANSWER, 1.0 - odds, MODEL, "%dms" % elapsed)
        # The delegator's builds the words do not ask for are where all of its
        # expensive mistakes were: "write me a haiku about compilers", "the
        # build is finished". Answered; a build missed this way is the cheap
        # mistake, and a chat turn can still write the files.
        if self.guarded and keyword_decide(message, files).kind != BUILD:
            return Decision(ANSWER, 1.0 - odds, MODEL,
                            "%dms, a build by the delegator but not by its words" % elapsed)
        return Decision(BUILD, odds, MODEL, "%dms" % elapsed)


# --- the whole decision ------------------------------------------------------------


def decide(core, request):
    """Answer or build, for a message as the core sends it: its "prompt", the
    names of its "attachments", and whether the folder it is about has
    "files" in it.

    The delegator is asked only when it is here or about to be wanted: one
    loaded on demand is not loaded over an expert chosen in the box for this
    alone (see the core's delegator.ready), and keywords decide instead.
    """
    message = message_of(request.get("prompt") or "", request.get("attachments") or [])
    files = bool(request.get("files"))
    method = request.get("method", "")
    if "keywords" in set(method.split(",")):
        return keyword_decide(message, files)
    ready = core.call("delegator.ready", {"optional": True}) or {}
    try:
        if not ready.get("available"):
            return keyword_decide(message, files)
        try:
            decider = Decider(core, ready.get("path", ""),
                              float(request.get("calibration", CALIBRATION)), method)
            return decider.decide(message, files, float(request.get("build_at", BUILD_AT)))
        except Disconnected:
            raise
        except Exception as error:   # the delegator could not be had after all
            decision = keyword_decide(message, files)
            decision.detail = "the delegator failed (%s), used keywords" % error
            return decision
    finally:
        let_go(core)


def odds(core, request):
    """The delegator's odds that a message is a build, with each order's
    scores, for crucible-routebench --decide: the numbers a line is drawn
    from."""
    message = message_of(request.get("prompt") or "", request.get("attachments") or [])
    ready = core.call("delegator.ready") or {}
    try:
        if not ready.get("available"):
            raise ValueError("there is no delegator to ask")
        decider = Decider(core, ready.get("path", ""), float(request.get("calibration", CALIBRATION)),
                          request.get("method", ""))
        orders = []
        chance = decider.build_odds(message, bool(request.get("files")), orders)
    finally:
        let_go(core)
    if not isinstance(chance, float):
        raise ValueError("the delegator could not score that message")
    return {"build": chance, "orders": orders}
