# SPDX-License-Identifier: MIT
"""Deciding which expert answers.

Two ways to decide, behind one function. The delegator model, which is the real
thing: it is asked to continue the conversation with each seat's name, and the
likeliest name wins. And keywords, which need no model at all -- for a roster
with no delegator, or a delegator that could not be loaded.

The model is the core's. This side writes the delegator's prompt, asks the core
to render it in the model's own chat format and to score the labels against it,
and does the arithmetic. Everything about *how* a route is chosen is here, so it
can be changed and measured without rebuilding anything; everything about the
weights stays where the weights are.

Measured with crucible-routebench: on Crucible's 54-prompt benchmark, 50/54
with LFM2.5-1.2B and 54/54 with gpt-oss-20b; on a five-seat roster of fifty
prompts, 49/50 with LFM -- a pH question sits on its line between Chemistry
and Mathematics -- and 48/50 with gpt-oss. See ModelRouter and Roster.orders for why three
orders rather than one.
"""

import math
import time

# Where a decision came from, by the names the session history stores.
MODEL = "router model"
KEYWORD = "keywords"
FALLBACK = "fallback"
PINNED = "pinned"

# How much of the delegator's measured lean to take out. With two worked
# examples per subject the benchmark no longer separates 0, 0.25 and 0.5 --
# all score 50/54 -- and a quarter is the one that held up best with one.
CALIBRATION = 0.25

# What a question with no content looks like. Whatever the delegator answers to
# these is not about a question, because there is none: it is its prior.
CONTENT_FREE = ("N/A", "", "...")


class Decision:
    __slots__ = ("expert", "confidence", "source", "detail")

    def __init__(self, expert="", confidence=0.0, source=FALLBACK, detail=""):
        self.expert = expert
        self.confidence = confidence
        self.source = source
        self.detail = detail

    def to_json(self):
        return {"expert": self.expert, "confidence": self.confidence,
                "source": self.source, "detail": self.detail}


# --- the roster, and what the delegator is shown of it -------------------------


class Roster:
    """The seats, in the order the side menu draws them."""

    def __init__(self, experts):
        self.experts = [dict(e) for e in experts or []]

    def label(self, expert_id):
        """The name on the roster, or the id when it is not on it."""
        for expert in self.experts:
            if expert.get("id") == expert_id:
                return expert.get("name") or expert_id
        return expert_id

    def labels(self):
        # The name, not the tag. Worth 39 points on the benchmark: "PHIL" and
        # "PHYS" are two spellings sharing a first token; "Philosophy" and
        # "Physics" are things the model knows about.
        return [e.get("name", "") for e in self.experts]

    def ids(self):
        return [e.get("id", "") for e in self.experts]

    def orders(self, many=3):
        """The orders the seats are shown in, each scored and the scores averaged.

        One order is not enough. A delegator leans towards a seat for where it
        sits in the list as well as for what it is: gpt-oss-20b, shown five
        seats in one order, sent biology questions to Chemistry, fourth on the
        list, and scored 39/50; the same delegator shown three orders -- as
        listed, reversed, and rotated by half -- scored 49/50, and 54/54 on the
        nine-seat benchmark. LFM2.5-1.2B went from 45 to 49 of 50. Each order
        costs a forward pass, which is the price of not trusting the first.
        """
        n = len(self.experts)
        listed = list(range(n))
        if many <= 1 or n < 2:
            return [listed]
        out = [listed, listed[::-1]]
        if many >= 3 and n > 2:
            out.append(listed[n // 2:] + listed[:n // 2])
        return out

    def system_prompt(self, order=None):
        # Routable seats only, and never a way to decline: a closing line
        # naming a catch-all was answered for almost everything (16%). Asked
        # for the subject's name, which is what is scored; the tag stays beside
        # it, where reading the list as a labeled menu is worth 6 points.
        prompt = ("You label a question with the one subject it belongs to.\n"
                  "Reply with only the subject's name.\n\n")
        for i in order if order is not None else range(len(self.experts)):
            e = self.experts[i]
            prompt += "%s  %s: %s\n" % (e.get("tag", ""), e.get("name", ""), e.get("blurb", ""))
        return prompt

    def examples(self, order=None):
        # The answer to each is exactly a label and nothing more: an example
        # that showed anything else would teach the delegator to keep writing
        # past the string being scored.
        pairs = []
        for i in order if order is not None else range(len(self.experts)):
            e = self.experts[i]
            for question in e.get("examples") or []:
                if question:
                    pairs.append((question, e.get("name", "")))
        return pairs


def answer_prefix(rendered):
    """What must follow the assistant header before a label is the next token.

    Harmony (gpt-oss) opens the assistant's turn with a channel marker, so the
    header is followed by `<|channel|>` and never by a word. Nine names scored
    there are nine things that cannot happen: measured, gpt-oss as delegator
    scored 13% and put 52 of 54 prompts in one seat. With the header completed
    it reads the question instead.
    """
    if rendered.endswith("<|start|>assistant"):
        return "<|channel|>final<|message|>"
    return ""


# --- keywords ----------------------------------------------------------------


def _ascii_lower(text):
    return "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in text)


def _is_word_char(c):
    return (c.isascii() and c.isalnum()) or c in "+#_"


def _count_whole_words(haystack, needle):
    """Whole-word matches only. Substrings send "function" to Chemistry, on "ion"."""
    if not needle:
        return 0
    found = 0
    pos = haystack.find(needle)
    while pos != -1:
        after = pos + len(needle)
        left_ok = pos == 0 or not _is_word_char(haystack[pos - 1])
        right_ok = after >= len(haystack) or not _is_word_char(haystack[after])
        if left_ok and right_ok:
            found += 1
        pos = haystack.find(needle, pos + 1)
    return found


def keyword_route(prompt, roster):
    haystack = _ascii_lower(prompt)
    scores = []
    for e in roster.experts:
        score = 0
        for word in e.get("keywords") or []:
            if word:
                score += _count_whole_words(haystack, _ascii_lower(word))
        scores.append(score)
    total = sum(scores)

    if not scores:
        return Decision(detail="no experts configured")
    best_score = max(scores)
    if best_score == 0:
        # Nothing matched, so nobody decided. The policy says where that goes.
        return Decision(detail="no expert keywords matched")
    best = scores.index(best_score)
    # This seat's share of all the matches: a prompt that hits three fields
    # at once reports the ambiguity. Never above 0.95 -- keywords are not proof.
    confidence = min(0.95, best_score / total) if total > 0 else 0.30
    return Decision(roster.experts[best].get("id", ""), confidence, KEYWORD,
                    "%d keyword match%s" % (best_score, "" if best_score == 1 else "es"))


# --- the delegator -------------------------------------------------------------


class ModelRouter:
    """Asks the delegator, through the core, which seat a prompt belongs to."""

    # The measured lean of a delegator, by the file and the prompt it was
    # measured with. A few forward passes is nothing once, and a third of the
    # work again on every prompt when the delegator is loaded and freed for
    # each one; it depends on neither the load nor the question.
    _bias = {}

    def __init__(self, core, roster, delegator_path, calibration=CALIBRATION, method=""):
        self.core = core
        self.roster = roster
        self.path = delegator_path
        self.calibration = calibration
        self.labels = roster.labels()
        # "single" scores the one order, for crucible-routebench to compare.
        self.orders = roster.orders(1 if method == "single" else 3)

    def conversation(self, question, order):
        # The worked examples as real turns: pasted into the system prompt
        # instead, the 1.2B delegator scored 42% against 74%.
        messages = [{"role": "system", "content": self.roster.system_prompt(order)}]
        for example, answer in self.roster.examples(order):
            messages.append({"role": "user", "content": example})
            messages.append({"role": "assistant", "content": answer})
        messages.append({"role": "user", "content": question})
        rendered = self.core.call("delegator.format", {"messages": messages})["text"]
        return rendered + answer_prefix(rendered)

    def score(self, question, order, cancelable=True):
        """Raw label scores for one question, or None when it could not be scored."""
        reply = self.core.call("delegator.score",
                               {"prompt": self.conversation(question, order), "labels": self.labels,
                                "cancelable": cancelable})
        scores = reply.get("scores") or []
        if reply.get("canceled"):
            return "canceled"
        if len(scores) != len(self.labels) or scores[0] is None:
            return None
        return [float(s) for s in scores]

    def bias(self, order):
        key = (self.path, self.roster.system_prompt(order), tuple(self.roster.examples(order)),
               tuple(self.labels))
        if key not in ModelRouter._bias:
            total = [0.0] * len(self.labels)
            measured = 0
            for question in CONTENT_FREE:
                # Not stopped by Stop: a lean half measured is a wrong one.
                scores = self.score(question, order, cancelable=False)
                if not isinstance(scores, list):
                    continue
                total = [t + s for t, s in zip(total, scores)]
                measured += 1
            # Uncalibrated is better than wrong.
            ModelRouter._bias[key] = [t / measured for t in total] if measured else [0.0] * len(total)
        return ModelRouter._bias[key]

    def adjusted(self, prompt):
        """Scores less a share of the lean, averaged over the orders; or why there are none."""
        total = [0.0] * len(self.labels)
        for order in self.orders:
            bias = self.bias(order)
            scores = self.score(prompt, order)
            if not isinstance(scores, list):
                return scores
            total = [t + s - self.calibration * b for t, s, b in zip(total, scores, bias)]
        return [t / len(self.orders) for t in total]

    def route(self, prompt):
        if not self.labels:
            return Decision(detail="no experts on the roster")
        for order in self.orders:
            self.bias(order)

        started = time.monotonic()
        adjusted = self.adjusted(prompt)
        elapsed = int((time.monotonic() - started) * 1000)
        if adjusted == "canceled":
            return Decision(detail="routing canceled")
        if adjusted is None:
            # A decode failure, or a context too small for the prompt.
            decision = keyword_route(prompt, self.roster)
            decision.detail = "delegator could not be scored, used keywords"
            return decision

        highest = max(adjusted)
        best = adjusted.index(highest)
        total = sum(math.exp(s - highest) for s in adjusted)
        confidence = min(1.0, max(0.0, 1.0 / total)) if total > 0 else 0.0
        return Decision(self.roster.ids()[best], confidence, MODEL, "%dms" % elapsed)


# --- what to do about it ---------------------------------------------------------


def apply_policy(decision, roster, seats, routing):
    """Decide what happens when the chosen seat cannot answer, or was a coin flip.

    `seats` says, for each expert id, whether it has a model behind it.
    """
    def has_model(expert_id):
        return bool(expert_id) and bool((seats.get(expert_id) or {}).get("model"))

    chosen_default = (routing or {}).get("default_expert") or ""
    backstop = chosen_default if has_model(chosen_default) else ""
    floor = float((routing or {}).get("min_confidence") or 0.0)

    # Unsure is treated as undecided and sent to the default expert. A pin
    # skips this: the user decided, not the model. With no default, the
    # delegator's best guess stands and the detail records the doubt.
    if decision.source == MODEL and floor > 0.0 and decision.confidence < floor:
        wanted = roster.label(decision.expert)
        doubt = "undecided (%s at %d%%)" % (wanted, int(decision.confidence * 100))
        if backstop and backstop != decision.expert:
            decision.detail = doubt + "; used " + roster.label(backstop)
            decision.expert = backstop
            decision.source = FALLBACK
            return decision
        decision.detail = doubt if not decision.detail else doubt + "; " + decision.detail

    if has_model(decision.expert):
        return decision

    # The chosen seat has no model -- or nothing was chosen at all, which is
    # what keywords give a prompt that has none of them.
    missed = ("no expert was named" if not decision.expert
              else roster.label(decision.expert) + " has no model")
    if backstop:
        decision.expert = backstop
        decision.source = FALLBACK
        decision.detail = missed + "; used " + roster.label(backstop)
        return decision

    # Any filled seat rather than failing, said plainly: the wrong specialist's
    # answer is worth more than a refusal, as long as the transcript admits it.
    available = [i for i in roster.ids() if has_model(i)]
    if not available:
        decision.detail = "no experts have a model"
        return decision
    decision.expert = available[0]
    decision.source = FALLBACK
    decision.detail = missed + "; used " + roster.label(decision.expert)
    return decision


def route(core, request):
    """The whole decision, from a request the core sends: see `handle_route`."""
    roster = Roster(request.get("roster"))
    seats = request.get("seats") or {}
    routing = request.get("routing") or {}
    prompt = request.get("prompt") or ""

    pinned = request.get("pinned") or ""
    if pinned:
        # A pinned route needs no delegator at all, so with the delegator set
        # to load on demand a pinned prompt costs nothing to route.
        decision = Decision(pinned, 1.0, PINNED, "pinned by slash command")
        return apply_policy(decision, roster, seats, routing)

    ready = core.call("delegator.ready") or {}
    if ready.get("available"):
        calibration = float(request.get("calibration", CALIBRATION))
        decision = ModelRouter(core, roster, ready.get("path", ""), calibration,
                               request.get("method", "")).route(prompt)
    else:
        decision = keyword_route(prompt, roster)
    return apply_policy(decision, roster, seats, routing)


def explain(core, request):
    """The arithmetic behind one decision, for crucible-routebench --explain:
    the only way to tell a delegator that dislikes a subject from a calibration
    that is taking it away."""
    roster = Roster(request.get("roster"))
    ready = core.call("delegator.ready") or {}
    if not ready.get("available"):
        raise ValueError("there is no delegator to explain")
    calibration = float(request.get("calibration", CALIBRATION))
    router = ModelRouter(core, roster, ready.get("path", ""), calibration, request.get("method", ""))
    order = router.orders[0]
    bias = router.bias(order)
    raw = router.score(request.get("prompt") or "", order, cancelable=False)
    if not isinstance(raw, list):
        raise ValueError("the delegator could not score that prompt")
    return {"labels": router.labels, "raw": raw, "bias": bias,
            "calibrated": [s - calibration * b for s, b in zip(raw, bias)]}
