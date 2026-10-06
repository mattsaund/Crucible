# SPDX-License-Identifier: MIT
"""Two short things the delegator writes: a conversation's name, and the worked
examples a new expert is routed by.

Both are the delegator's because it is small and loaded more often than
anything else, and both are a few words of reading and saying, which it does
well. The core loads it and generates; what to ask and how to read the answer
is here.
"""

import re


# --- naming a conversation ------------------------------------------------------


def naming_prompt(excerpt):
    """Examples in the prompt rather than rules alone: a small model told "a
    short title" writes a sentence, and told "two to four words" writes four
    words of the question."""
    return ("Here is the start of a conversation:\n\n" + excerpt
            + "\n\nGive it a title of two to four words saying what it is about, like a "
              "heading -- for example: Math homework, Fixing a Python import, Trip to Lisbon, "
              "Orbital speed. Reply with the title and nothing else.")


def name_from(reply):
    """A title out of what a model said: the first line, without the quotes, the
    "Title:", the bold or the full stop a model adds -- or "" when what it said
    was a sentence rather than a title."""
    line = ""
    for candidate in (reply or "").splitlines():
        if candidate.strip():
            line = candidate.strip()
            break
    for lead in ("Title:", "title:", "TITLE:"):
        if line.startswith(lead):
            line = line[len(lead):].strip()
    clean = "".join(c for c in line if c not in "\"*#`").strip()
    clean = clean.rstrip(".!:")
    if len(clean) >= 2 and clean[0] == "'" and clean[-1] == "'":
        clean = clean[1:-1]
    # Six words at most: a title, not a sentence. A model that wrote one gave
    # no title, and the fallback is better than its first six words.
    words = clean.split()
    if not words or len(words) > 6 or len(clean) > 48:
        return ""
    return clean[0].upper() + clean[1:]


def fallback_name(excerpt):
    """The opening words of the first prompt, for when no model can be asked:
    at most five, up to the first full stop or question mark."""
    first = (excerpt or "").split("\n", 1)[0]
    if first.startswith("Question: "):
        first = first[len("Question: "):]   # how the window hands the prompt over
    end = re.search(r"[.?!]", first)
    if end and end.start() > 0:
        first = first[:end.start()]
    out = " ".join(first.split()[:5])
    return out[:1].upper() + out[1:] if out else ""


def name_session(core, params):
    excerpt = params.get("excerpt", "")
    name = ""
    ready = core.call("delegator.ready") or {}
    if ready.get("available"):
        reply = core.call("delegator.generate", {
            "messages": [{"role": "user", "content": naming_prompt(excerpt)}],
            "temperature": 0.2, "max_tokens": 24}) or {}
        name = name_from(reply.get("text", ""))
    return {"name": name or fallback_name(excerpt)}


# --- an expert's worked examples ---------------------------------------------------


def examples_prompt(name, blurb):
    """Bare questions and nothing else. Every rule here is there because a
    model broke it: they number lists, explain first, answer their own
    question, and drift towards the general ("what is chemistry")."""
    return ('An expert called "' + name + '" handles: ' + blurb
            + "\n\nWrite exactly two short questions a person would ask that this expert "
              "should obviously answer.\n"
              "Rules: one question per line. No numbering, no bullets, no quotes, no "
              "explanation. Each question must be specific enough that it could not be "
              "asked of a different expert.\n")


def parse_examples(reply, wanted=2):
    questions = []
    for line in (reply or "").splitlines():
        if len(questions) >= wanted:
            break
        text = line.strip()
        if not text:
            continue
        # The decoration models add whatever they were told: "1. ", "- ", "* ",
        # "Q: ", and surrounding quotes.
        text = text.lstrip("0123456789.)-* ").strip()
        if len(text) > 2 and text[:2] in ("Q:", "A:"):
            text = text[2:].strip()
        if len(text) >= 2 and text[0] == '"' and text[-1] == '"':
            text = text[1:-1].strip()
        # A line of preamble ("Here are two questions:") ends in a colon and is
        # not a question; a real one is long enough to be worth showing a model.
        if len(text) < 12 or text.endswith(":"):
            continue
        questions.append(text)
    return questions


def write_examples(core, params):
    ready = core.call("delegator.ready") or {}
    if not ready.get("available"):
        return {"examples": []}   # no delegator: the seat routes on its blurb and keywords
    # Sampled, and warmer than routing: two questions that are near-copies of
    # each other teach the delegator nothing, and greedy decoding on a short
    # prompt produces exactly that.
    reply = core.call("delegator.generate", {
        "messages": [{"role": "user",
                      "content": examples_prompt(params.get("name", ""), params.get("blurb", ""))}],
        "temperature": 0.6, "max_tokens": 128}) or {}
    return {"examples": parse_examples(reply.get("text", ""))}
