# SPDX-License-Identifier: MIT
"""A chat turn: a question, the expert's answer, and the tools in between.

A prompt typed into Chat -- or into the Test window in Create -- is first
decided: a request to make or change software becomes a build, which the
core starts on the turn's own words, and the turn is the build's from there
(see intent.py). Anything else is routed, the expert that wins is seated, and
it answers. Ordinarily that is one round and the end of it. When the expert
reaches for a tool instead -- READ a file, RUN the tests, SEARCH the web -- the
tool is run, what came of it goes back, and the expert goes round again, until
it writes an answer for the person who asked or the turn runs out of rounds.

Three things are worth knowing before reading it.

The transcript is the person's, and it is kept readable while the turn works.
A round that reached for a tool said two things: a sentence for the reader and
a line for the machine. The line comes off the reply and the sentence stays --
it is the only explanation of the change that will ever be written -- and what
the tool did goes above the reply as an action, with the diff it made or the
output it printed.

A turn is bounded, because a turn that never writes an answer is a turn the
person who asked gets nothing from. A dozen actions at most, the same call
never run twice, a search asked for again refused, and the last round told it
is the last. A turn that ends with a call where its answer should be says why
in words instead.

The core does what only the core may: it seats the expert and holds its model
for the turn, keeps the conversation the turn belongs to and fits it to the
expert's context, streams each round into the transcript as it is written,
runs the tools inside the trusted folder, searches the web, and puts an edit to
the person. Which of those happens, in what order, and what the expert is told
about each, is decided here.
"""

import re
import sys
import traceback

from . import intent, routing
from .rpc import Disconnected

# How many times one chat turn may act on the project before it has to answer.
#
# Higher than the search ceiling and counting a different thing. Reading a
# file, rewriting it and running the tests is three rounds of honest work; the
# same number of searches is a model going round in circles. Bounded all the
# same, because a turn that never writes an answer is a turn the person who
# asked gets nothing from.
TOOL_ROUNDS = 12

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

# The verbs that act on the project, rather than answer a cook or search the
# web. SEARCH has a path of its own below. ASK, DONE and HANDOFF are how a cook
# ends a piece of work and are not offered to a chat turn at all; one that
# arrives anyway is shown, which is right -- a model that writes "DONE: fixed
# it" has said something to the reader.
PROJECT_VERBS = frozenset((
    "list", "read", "write", "edit", "find", "render", "tool", "run", "note", "fetch", "git",
    "gh", "python", "start", "stop", "logs", "screenshot", "click", "move", "type", "key",
    "scroll"))

ALREADY_DONE = ("You already did exactly that this turn, and its result is above. Use it, and "
                "if it told you what you needed, answer now.")

DECLINED = ("The user declined that edit; the file is unchanged. Do not try the same write "
            "again. Either explain what you were going to change and why, or propose "
            "something different.")

LAST_ACTION = ("\n\nThat was the last action available this turn. Write the answer now from "
               "what you have.")

LAST_SEARCH = ("\n\nThis was the last search available. Answer now from what you have, and say "
               "plainly if it is not enough.")

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


def system_prompt(params, seat):
    """The expert's instructions for this turn, from the person's own prompt
    and what the seated model can do."""
    text = params.get("system_prompt", "")
    effort = params.get("effort", "")
    if effort and not seat.get("remote") and seat.get("takes_effort"):
        # Where a local reasoning model looks for it. A provider takes it as a
        # field of the request instead, which the core sends with each round.
        text += "\n\nReasoning: " + effort
    text += MARKDOWN
    if params.get("web_search"):
        # Only when the tool is switched on. An expert told it can search when
        # it cannot will offer to, which is worse than not having the tool.
        text += params.get("search_tools", "")
    # The same tools a cook gets, in an ordinary chat turn. There used to be a
    # line here: cooking could change the project and chatting could only talk
    # about it. That was never a distinction anybody asked for -- "fix the typo
    # in README" is a sentence, not a goal worth starting a cook for, and the
    # answer to it is the edit. What separates the two is how long they run
    # and how they end, not what they may touch. The permission is the folder,
    # answered once, in the terms it is about: with none trusted, there are no
    # tools to list.
    return text + seat.get("tools", "")


def rounds_for(params, workshop):
    """How many rounds a turn may take.

    Ordinarily one: the expert answers and that is the end of it. A round only
    repeats when the expert asked to look something up, and the number of
    times it may is bounded -- an expert that reads results and searches again
    is being useful, one that does it eight times is stuck. With the workshop
    the ceiling is higher and a different thing is being counted: reading a
    file, changing it and running the tests is three rounds of honest work,
    where three searches is a model going in circles.
    """
    rounds = 1
    if params.get("web_search"):
        rounds = max(rounds, int(params.get("search_rounds") or 0) + 1)
    if workshop:
        rounds = max(rounds, TOOL_ROUNDS)
    return rounds


def dropped_note(dropped):
    """The transcript's line for exchanges left out to fit the context.
    `dropped` counts messages, which go in pairs."""
    return "dropped %d %s to stay inside the context" % (
        dropped // 2, "earlier exchange" if dropped == 2 else "earlier exchanges")


def unanswered(shown, parsed, hit_limit):
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
    if shown and not asked_to_act and not asked_to_search:
        return ""
    if asked_to_act:
        return "ran out of turns before finishing. Ask again to carry on"
    if asked_to_search:
        return 'kept searching instead of answering. Raise "Searches per prompt" in Settings, Tools'
    if hit_limit:
        return ('thought until "Longest reply" ran out. Raise it in Settings, Generation, or '
                'lower the effort')
    return "stopped without answering"


# --- the turn ---------------------------------------------------------------------


class Turn:
    """One turn, from its route to its end. Every call it makes of the core
    names the turn, which the core opened before asking for it."""

    def __init__(self, core, params):
        self.core = core
        self.params = params
        self.index = params.get("turn", 0)
        self.roster = routing.Roster(params.get("roster"))

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

        # --- what it becomes ---------------------------------------------------
        #
        # Before who answers, because a build needs nobody to answer: its
        # architect is chosen for the plan, and its agents for their tasks.
        # The core says when a build cannot start (one is running already),
        # and then the message is a turn like any other.
        if params.get("can_build") and self.became_a_build():
            return

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
        name = self.roster.label(decision.expert)
        seat = self.call("turn.load")
        if not seat.get("ok"):
            error = seat.get("error", "")
            if error == "stopped":
                # A load the person stopped is not an error to be explained,
                # and the turn it belonged to reads as canceled, not failed.
                self.end("canceled")
            else:
                self.end("failed", reason=error, mood="error", status=error)
            return
        self.mood("thinking", name + " is reading")

        # --- the conversation ----------------------------------------------------
        #
        # What was attached goes above what was typed, so the question comes
        # after the material it is about -- the order every model reads best.
        # The core reads it for the seated model: how much of a long document
        # fits is a question about this expert's context.
        if params.get("attachments"):
            self.mood("thinking", name + " is reading the attachments")
        opened = self.call("turn.open", {"system": system_prompt(params, seat),
                                         "history": HISTORY_TURNS * 2, "share": PROMPT_SHARE})

        # Does it still fit? Asked once the expert is seated, because only the
        # expert can answer it: the context is the one it was loaded with and
        # the count is its own tokenizer's.
        size = int(opened.get("size") or 0)
        used = int(opened.get("used") or 0)
        budget = int(size * PROMPT_SHARE)
        policy = params.get("overflow", "")
        if policy == "stop" and used > budget:
            # Refused rather than shortened. The model cannot tell you what it
            # stopped being able to see, so the program says it instead.
            reason = ('this conversation no longer fits: %d of %d tokens. Raise "Context" in '
                      'Settings, Generation, or start a new chat.' % (used, size))
            self.end("failed", reason=reason, mood="error", status="context full")
            return
        dropped = int(self.call("turn.fit", {"policy": policy, "budget": budget}).get("dropped") or 0)
        if dropped > 0:
            self.show(actions=[{"summary": dropped_note(dropped)}])

        self.rounds(seat)

    def became_a_build(self):
        """Decide whether this message is a build, and start it when it is.

        True when the turn is over because a build has it now. A decision
        that could not be made, a Stop while it was, or a build the core
        would not start leaves the message to be answered, which a turn can
        always do.
        """
        params = self.params
        try:
            decided = intent.decide(self.core, {"prompt": params.get("prompt", ""),
                                                "attachments": params.get("attachments") or [],
                                                "files": bool(params.get("files"))})
        except Disconnected:
            raise
        except Exception as error:   # the delegator's calls failed outright
            print("chat: the message could not be decided (%s), so it is answered" % error,
                  file=sys.stderr)
            return False
        if decided.kind != intent.BUILD or self.flags().get("cancel"):
            return False
        started = self.call("turn.build", {"decision": decided.to_json()})
        if not started.get("ok"):
            print("chat: the build was not started (%s), so it is answered" % started.get("error", ""),
                  file=sys.stderr)
            return False
        # Over for the chat. What the build leaves becomes the reply, and is
        # remembered, when it ends; the status line is the build's meanwhile.
        self.end("finished", status="building")
        return True

    def rounds(self, seat):
        """Ask, and act on what the expert reached for, until it answers."""
        params = self.params
        workshop = bool(seat.get("workshop"))
        rounds = rounds_for(params, workshop)
        exchanges = []         # this turn's own messages, after the conversation it opened with
        earlier = ""           # what the rounds before this one left on screen
        already_done = []      # the project calls made this turn
        already_searched = []
        canceled = hit_limit = False
        output_tokens = 0
        shown = ""

        for round_ in range(rounds):
            # A turn that reads a file, then writes one, then answers says
            # something each time, and each is kept -- a paragraph apart,
            # rather than run into the next sentence or replaced by it.
            said = self.call("turn.round", {"messages": exchanges, "paragraph": bool(earlier)})
            answer = said.get("answer", "")
            reasoning = said.get("reasoning", "")
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
                    # A reply that had started is kept -- but then nothing would
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
            if canceled or round_ + 1 >= rounds:
                break

            last = round_ + 2 >= rounds
            parsed = self.core.call("tools.parse", {"answer": answer, "reasoning": reasoning}) or {}

            # --- did it reach for the project? ----------------------------------
            #
            # Before the search, because SEARCH is one of the verbs the parser
            # knows and the search below has bookkeeping of its own -- repeats,
            # the last-search warning -- that the file verbs do not want.
            call = parsed.get("call") if workshop else None
            if call and call.get("kind") in PROJECT_VERBS:
                earlier = self.act(call, answer, earlier, already_done, exchanges, last)
                continue

            query = parsed.get("search") or ""
            if not query:
                break   # it answered, which is the ordinary case
            self.search(query, answer, earlier, already_searched, exchanges, last)

        # --- the end of the turn ------------------------------------------------
        #
        # Only an exchange that produced an answer is remembered, so a canceled
        # turn -- or one that spent its whole budget thinking -- does not poison
        # the context of the next one. Remembered before a placeholder is put
        # in its place: what goes into the history is what the model said, not
        # what Crucible said about it.
        answered = not canceled and output_tokens > 0
        why = ""
        if answered:
            parsed = (self.core.call("tools.parse", {"answer": shown, "reasoning": ""}) or {}) if shown else {}
            why = unanswered(shown, parsed, hit_limit)
        # Finished only now, with the reply it will keep: the session is saved
        # as a turn finishes, and a turn finished before its placeholder saved
        # the raw call, which came back on resume.
        self.end("finished", status="canceled" if canceled else "", remember=answered and bool(shown),
                 reply="(" + why + ")" if why else None)

    def act(self, call, answer, earlier, already_done, exchanges, last):
        """Run the project call `call` the expert made, put what came of it
        above the reply, and hand the result back. The reply as it now reads."""
        kind = call.get("kind", "")
        argument = call.get("argument", "")
        self.mood("thinking", doing(kind, argument))

        # The turn so far, and this round's prose up to the call.
        kept = paragraphs(earlier, prose_before_tool_call(answer, kind))

        # The same call twice in one turn is a model going round, and the
        # second answer would be the first one again. Not run, and not drawn:
        # it is told its result is already above. A write or an edit is the
        # exception -- changing a file again is how a fix to it is made.
        changes_a_file = kind in ("write", "edit")
        asked_for = kind + "\n" + argument
        if not changes_a_file and asked_for in already_done:
            self.show(reply=kept)
            exchanges.append({"role": "assistant", "content": answer})
            exchanges.append({"role": "user", "content": ALREADY_DONE})
            return kept
        already_done.append(asked_for)

        # --- the gate ---------------------------------------------------------
        #
        # A write or an edit is the call that changes something the person
        # owns, so it is the one that stops and asks -- unless they have said
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
                exchanges.append({"role": "assistant", "content": answer})
                exchanges.append({"role": "user", "content": DECLINED})
                return earlier

        result = self.call("turn.tool", {"call": call})
        # The diff a write made, or the output a command printed, kept for the
        # transcript rather than only handed to the model -- and the protocol
        # line off the reply, with the prose around it staying.
        self.show(actions=[{"summary": result.get("summary", ""), "body": result.get("detail", ""),
                            "language": argument if changes_a_file else "",
                            "picture": result.get("picture", "")}],
                  reply=kept)
        exchanges.append({"role": "assistant", "content": answer})

        # A picture the tool produced goes to a model that can see one, as a
        # picture, and with every round after this one; one that cannot is
        # told so, beside whatever words were read off it.
        handback = {"role": "user", "content": result.get("output", "")}
        if result.get("pictures"):
            if result.get("sees"):
                handback["pictures"] = result["pictures"]
            else:
                handback["content"] += TEXT_ONLY
        if last:
            handback["content"] += LAST_ACTION
        exchanges.append(handback)
        return kept

    def search(self, query, answer, earlier, already_searched, exchanges, last):
        """Look up what the expert asked for, and hand back what was found.

        Asking twice for the same thing is a model that has read the results
        and not known what to do with them. Running the search again would hand
        back the same page and invite it to do the same, so it is told instead
        -- and that becomes its last search.
        """
        repeat = query in already_searched
        already_searched.append(query)
        self.mood("thinking", ('already searched for "%s"' if repeat else 'searching for "%s"') % query)

        actions = []
        if repeat:
            handback = 'You have already searched for "%s" and been given the results above.' % query
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
        exchanges.append({"role": "assistant", "content": answer})
        if repeat or last:
            # The last search it is allowed. Saying so is the difference
            # between an answer and a model that asks to search again and ends
            # the turn with nothing in it.
            handback += LAST_SEARCH
        exchanges.append({"role": "user", "content": handback})


def turn(core, params):
    """Answer one chat turn, from its route to its end."""
    chat = Turn(core, params)
    try:
        chat.run()
    except Disconnected:
        raise
    except Exception as error:   # a call to the core that failed, or a bug here
        # The turn ends with the reason rather than being left open with
        # nobody answering it.
        traceback.print_exc(file=sys.stderr)
        try:
            chat.end("failed", reason=str(error), mood="error", status=str(error))
        except Exception:   # the core closes a turn left open itself
            pass
    return {}
