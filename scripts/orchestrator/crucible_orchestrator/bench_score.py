# SPDX-License-Identifier: MIT
"""Checking a model's answers to crucible-bench-1.

bench.py asks the questions; this decides whether what came back is right. The
letter a multiple-choice answer picks, the number a word problem settles on,
whether two pieces of LaTeX say the same thing, whether a program passes its
tests, and whether a reply did what IFEval told it to. The caller passes the
final answer only, with any thinking already taken out.

Everything here reads text a model wrote, so the readers are lenient about
decoration (bold, dollar signs, a trailing period) and strict about content. The
one function that runs anything, run_python, runs it in a folder of its own,
under limits, and cleans up after it.
"""

import json
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import textwrap
import time
import unicodedata
from functools import lru_cache

try:
    import resource
except ImportError:   # Windows: a job object holds the program instead (_windows_job)
    resource = None


def _has_alnum(text):
    return any(c.isalnum() for c in text)


# --- multiple choice -----------------------------------------------------------

# Where a model says which option it picks. The letter comes after each one.
_CHOICE_CUES = re.compile(
    r"\banswer\b[\s*_]*(?:(?:is|was|would\s+be|should\s+be|will\s+be|must\s+be)\b|[:=])"
    r"|\b(?:correct|right|best)\s+(?:answer|option|choice|letter)\b[\s*_]*(?:(?:is|would\s+be)\b|[:=])"
    r"|\bI(?:\s+would|\s+will|'d|'ll|\u2019d|\u2019ll)?\s+(?:choose|pick|select|go\s+with)\b"
    r"|\\boxed(?![A-Za-z])",
    re.IGNORECASE)

# The letter before the cue: "Option B is correct", "(B) is the right one".
# A bare letter counts only at the start of a sentence, so that "only I is
# correct" (a numeral in a list of statements) is not read as option I.
_LETTER_IS_RIGHT = re.compile(
    r"(?:\b(?:option|choice)\s+\(?([A-Za-z0-9])\)?|\(([A-Za-z0-9])\)|(?:^|(?<=[.!?:]\s))\**([A-Z])\**)"
    r"\s+is\s+(?:the\s+)?(?:correct|right|best)\b",
    re.IGNORECASE | re.MULTILINE)

# What may stand between a cue and its letter: space, markdown, brackets,
# quotes, LaTeX wrappers, and the words a model puts in front of a letter
# ("the answer is clearly option B").
_LETTER_LEAD = re.compile(
    r"(?:[\s*_$:(\[{'\"\u201c\u2018]"
    r"|\\(?:boxed|text|textbf|textit|textrm|mathrm|mathbf)(?![A-Za-z])"
    r"|\b(?:option|choice|letter|clearly|therefore|thus|definitely|probably|likely|indeed"
    r"|actually|simply|obviously)\b)*",
    re.IGNORECASE)

# Words that can follow an answer letter in a sentence: "The answer is A because".
# After "A" or "I" any other word means the article or the pronoun ("Answer: A
# good question", "the answer is I think B"), and that is not a choice.
_AFTER_LETTER = frozenset(("because", "since", "as", "is", "and", "or", "but", "which",
                           "here", "in", "given", "seems", "appears", "looks"))

# A line that is nothing but a letter: "C", "(C)", "C.", "C)", "**C**".
_LETTER_LINE = re.compile(
    r"^[ \t>#*_]*(?:(?:option|choice)\s+)?[(\[]?([A-Za-z0-9])[)\]]?[.:]?[ \t*_]*$",
    re.IGNORECASE | re.MULTILINE)

# A line that starts with an option's label: "B) London", "(B) London", "B. London".
_LETTER_MARK = re.compile(r"^[ \t*_]*(?:\(([A-Za-z0-9])\)|([A-Za-z0-9])[).:])[ \t*_]", re.MULTILINE)


def _letter_at(text, i, letters):
    """The option letter at text[i], or None when what is there is not one."""
    if i >= len(text):
        return None
    c = text[i]
    if not c.isascii() or not c.isalnum() or c.upper() not in letters:
        return None
    after = text[i + 1:i + 24]
    if re.match(r"\w|['\u2019-]\w", after):
        return None   # part of a word: "Because", "I'm", "A-level"
    if c.islower():
        # Lowercase only when it stands apart: "(c)", "c)", "c." at the end. "The
        # answer is a bit tricky" picks nothing.
        return c.upper() if not after or after[0] in ")].:,;}*$'\"\r\n" else None
    if c in "AI":
        follow = re.match(r"[ \t]+([a-z]+|\d)", after)
        if follow and follow.group(1) not in _AFTER_LETTER:
            return None
    return c


def choice_letter(text, letters):
    """Which option `text` picks, as one of `letters` ("ABCD"), or None.

    An explicit statement wins ("the answer is (C)", "Answer: C", "\\boxed{C}"),
    the last one when there are several, since a model that changes its mind
    says so later. Then a line holding only a letter, then a reply whose first
    line starts with an option's label ("B) London").
    """
    if not text or not letters:
        return None
    letters = "".join(letters).upper()

    stated = []
    for cue in _CHOICE_CUES.finditer(text):
        letter = _letter_at(text, _LETTER_LEAD.match(text, cue.end()).end(), letters)
        if letter:
            stated.append((cue.start(), letter))
    for match in _LETTER_IS_RIGHT.finditer(text):
        letter = next(g for g in match.groups() if g).upper()
        if letter in letters:
            stated.append((match.start(), letter))
    if stated:
        return max(stated)[1]

    alone = [m.group(1).upper() for m in _LETTER_LINE.finditer(text) if m.group(1).upper() in letters]
    if alone:
        return alone[-1]

    # Only when no other line starts with a label too: a reply that lists the
    # options before answering has one for every option.
    marks = list(_LETTER_MARK.finditer(text))
    if len(marks) == 1 and not text[:marks[0].start()].strip():
        letter = (marks[0].group(1) or marks[0].group(2)).upper()
        if letter in letters:
            return letter
    return None


# --- numbers: GSM8K ------------------------------------------------------------

# A number as people write one: a sign, a dollar sign, thousands separators,
# decimals. Never the tail of a word or of another number ("x2", "1.5.3").
_NUMBER = re.compile(
    r"(?<![\w.])([-\u2212]?)\$?((?:\d{1,3}(?:,\d{3})+|\d+)(?:\.\d+)?|\.\d+)(?!\d)")

# "#### 72", the way GSM8K's own solutions end. A markdown heading ("#### Step
# 3") is not followed by a number, so it is not mistaken for one.
_HASHES = re.compile(r"####[ \t]*(?=[-\u2212$\d.])")

_ANSWER_IS = re.compile(r"\banswer\b[\s*_]*(?:(?:is|was|would\s+be|will\s+be)\b|[:=])", re.IGNORECASE)


def _canonical(sign, digits):
    digits = digits.replace(",", "")
    whole, _, fraction = digits.partition(".")
    whole = whole.lstrip("0") or "0"
    fraction = fraction.rstrip("0")
    number = whole + ("." + fraction if fraction else "")
    return "-" + number if sign and number != "0" else number


def _numbers(text):
    return [_canonical(m.group(1), m.group(2)) for m in _NUMBER.finditer(text or "")]


def gsm8k_gold(answer_field):
    """The gold number in a GSM8K answer field: what follows "####", without commas."""
    if not answer_field or "####" not in answer_field:
        return None
    tail = answer_field.rsplit("####", 1)[1].strip()
    gold = tail.splitlines()[0] if tail else ""
    return gold.replace(",", "").strip() or None


def final_number(text):
    """The number a GSM8K answer settles on, normalized ("$1,080.00" is "1080"), or None.

    "#### N" first, then \\boxed{N}, then "the answer is N", then the last
    number in the text.
    """
    if not text:
        return None
    for hashes in reversed(list(_HASHES.finditer(text))):
        found = _numbers(text[hashes.end():].split("\n", 1)[0])
        if found:
            return found[0]
    boxed = last_boxed(text)
    if boxed:
        found = _numbers(boxed)
        if found:
            return found[-1]   # "\boxed{3 \times 6 = 18}" settles on 18
    for cue in reversed(list(_ANSWER_IS.finditer(text))):
        found = _numbers(text[cue.end():].split("\n", 1)[0])
        if found:
            return found[0]
    found = _numbers(text)
    return found[-1] if found else None


_PLAIN_NUMBER = re.compile(r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?")


def _plain_number(value):
    if value is None or isinstance(value, bool):
        return None
    text = re.sub(r"[\s,$%]", "", str(value)).replace("\u2212", "-")
    if not _PLAIN_NUMBER.fullmatch(text):
        return None
    number = float(text)
    return number if math.isfinite(number) else None


def same_number(a, b):
    """Whether two number strings are the same number: "72", "72.0" and "72.00" are.

    False when either is not a number at all.
    """
    x, y = _plain_number(a), _plain_number(b)
    if x is None or y is None:
        return False
    return math.isclose(x, y, rel_tol=1e-6, abs_tol=1e-9)


# --- MATH ----------------------------------------------------------------------

_BOX = re.compile(r"\\(?:boxed|fbox)(?![A-Za-z])")


def last_boxed(text):
    """What the last \\boxed{...} (or \\fbox{...}) holds, or None when there is none.

    Braces nest, so "\\boxed{\\frac{1}{2}}" holds "\\frac{1}{2}", and "\\boxed 5"
    holds "5". A box that never closes holds nothing: the answer was cut off.
    """
    if not text:
        return None
    boxes = list(_BOX.finditer(text))
    if not boxes:
        return None
    start = boxes[-1].end()
    i = start
    while i < len(text) and text[i] in " \t":
        i += 1
    if i < len(text) and text[i] == "{":
        depth = 0
        j = i
        while j < len(text):
            if text[j] == "\\":
                j += 2   # "\{" and "\}" are characters, not grouping
                continue
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    return text[i + 1:j].strip()
            j += 1
        return None
    bare = re.match(r"[ \t]+([^\s$]+)", text[start:])
    return bare.group(1).rstrip(".,;") if bare else None


_MATH_ANSWER_IS = re.compile(r"\banswer\b[\s*_]*(?:is\b|:)[\s*_:]*", re.IGNORECASE)

_INLINE_MATH = re.compile(
    r"(?<!\\)\$\$(.+?)(?<!\\)\$\$|(?<!\\)\$([^$\n]+?)(?<!\\)\$|\\\((.+?)\\\)|\\\[(.+?)\\\]",
    re.DOTALL)


def _math_span(match):
    return next(g for g in match.groups() if g is not None).strip()


def math_answer(text):
    """The final answer of a MATH solution: the last box, else what follows
    "answer is", else the last piece of inline math; None when there is none."""
    if not text:
        return None
    boxed = last_boxed(text)
    if boxed:
        return boxed
    stated = list(_MATH_ANSWER_IS.finditer(text))
    if stated:
        rest = text[stated[-1].end():].split("\n", 1)[0].strip()
        span = _INLINE_MATH.match(rest)
        if span:
            return _math_span(span)
        # "The answer is 0.5. I hope it is correct." ends at the sentence.
        value = re.split(r"\.(?:\s|$)", rest, maxsplit=1)[0].strip(" *")
        if value:
            return value
    spans = list(_INLINE_MATH.finditer(text))
    return _math_span(spans[-1]) if spans else None


_UNICODE_MATH = str.maketrans({
    "\u03c0": "\\pi", "\u221e": "\\infty", "\u2212": "-", "\u2013": "-", "\u00d7": "\\times",
    "\u00b7": "\\cdot", "\u2264": "\\le", "\u2265": "\\ge", "\u2260": "\\ne", "\u221a": "\\sqrt",
    "\u00b0": "^\\circ",
})

_TEXT_WRAPPER = re.compile(
    r"\\(?:text|textbf|textit|textrm|textnormal|mathrm|mathbf|mathit|mbox|operatorname)\s*\{([^{}]*)\}")

# Units an answer may carry that its gold does not, or the other way round:
# "5.4 \text{ cents}" is the number 5.4. Only words that cannot be a variable,
# so no single letters.
_UNITS = re.compile(
    r"(?<![A-Za-z\\])(?:(?:square|sq\.?|cubic)\s+)?"
    r"(?:units?|cm|mm|km|meters?|metres?|centimeters?|centimetres?|millimeters?|kilometers?"
    r"|inch(?:es)?|feet|foot|ft|yards?|yd|miles?|mph|seconds?|secs?|minutes?|mins?|hours?|hrs?"
    r"|days?|weeks?|months?|years?|dollars?|cents?|pounds?|lbs?|ounces?|oz|grams?|kg|kilograms?"
    r"|mg|liters?|litres?|ml|gallons?|degrees?)"
    r"(?:\s*\^\s*\{?\s*[23]\s*\}?)?(?![A-Za-z])")

# A fraction or a root with a short argument: "\frac12", "\frac{270}7", "\sqrt2".
_FRAC_OR_SQRT = re.compile(r"\\(frac|sqrt)(?![A-Za-z])")

# "14/3" and "\pi/2", for golds and answers that write fractions as \frac. A
# letter straight after a backslash is a command's, not a numerator.
_SLASH = re.compile(
    r"(?<![\w}.])(\d+(?:\.\d+)?|(?<!\\)[A-Za-z]|\\pi)/(\d+(?:\.\d+)?|[A-Za-z]|\\pi)(?![\w{.])")

# A leading variable: "x = 5", "x \in [-2, 7]", "\theta = 30".
_VARIABLE = re.compile(r"^(?:[A-Za-z]|\\[A-Za-z]+)(?:_\{?\w+\}?)?(?:=|\\in(?![A-Za-z]))")
_RELATION = re.compile(r"=|\\in(?![A-Za-z])")

# A number written with its base: "4210_5".
_BASED = re.compile(r"(\d+)_\d+")


def _read_argument(s, i):
    """One argument of \\frac or \\sqrt at s[i]: a {group}, a command or a character."""
    if i >= len(s):
        return None, i
    if s[i] == "{":
        depth = 0
        for j in range(i, len(s)):
            if s[j] == "{":
                depth += 1
            elif s[j] == "}":
                depth -= 1
                if depth == 0:
                    return s[i + 1:j], j + 1
        return None, i
    if s[i] == "\\":
        command = re.match(r"\\[A-Za-z]+", s[i:])
        width = command.end() if command else 2
        return s[i:i + width], i + width
    return s[i], i + 1


def _brace_arguments(s):
    """Every \\frac and \\sqrt with braced arguments: "\\frac12" is "\\frac{1}{2}"."""
    out = []
    i = 0
    while i < len(s):
        match = _FRAC_OR_SQRT.match(s, i)
        if not match:
            out.append(s[i])
            i += 1
            continue
        name = match.group(1)
        out.append(match.group(0))
        i = match.end()
        if name == "sqrt" and s[i:i + 1] == "[":
            close = s.find("]", i)
            if close != -1:
                out.append(s[i:close + 1])
                i = close + 1
        for _ in range(2 if name == "frac" else 1):
            argument, i = _read_argument(s, i)
            if argument is None:
                break
            out.append("{" + _brace_arguments(argument) + "}")
    return "".join(out)


def _math_norm(s):
    s = str(s).strip()
    if _BOX.match(s):
        inner = last_boxed(s)
        if inner is not None:
            s = inner
    s = s.translate(_UNICODE_MATH)
    s = re.sub(r"\\(?:left|right)\.", "", s)
    s = re.sub(r"\\(?:left|right|[bB]igg?[lr]?)(?![A-Za-z])", "", s)
    # Thin spaces go; "\\" (a matrix's new row) stays.
    s = re.sub(r"\\\\|\\[,;:! ]", lambda m: m.group(0) if m.group(0) == "\\\\" else " ", s)
    s = s.replace("~", " ").replace("\\displaystyle", "")
    s = re.sub(r"\\[dtc]frac(?![A-Za-z])", r"\\frac", s)
    s = re.sub(r"\^\s*\{?\s*\\circ\s*\}?|\\circ(?![A-Za-z])|\\degree(?![A-Za-z])", "", s)
    s = re.sub(r"\\?%", "", s)
    s = re.sub(r"\\?\$", "", s)
    for _ in range(3):   # a wrapper inside a wrapper: \textbf{\text{...}}
        s = _TEXT_WRAPPER.sub(lambda m: " " + m.group(1) + " ", s)
    # Units go from both sides, so that "864 \mbox{ inches}^2" is 864 and so is
    # "864 square inches". Unless that leaves nothing: then the word is the answer.
    without_units = _UNITS.sub(" ", s)
    if without_units.strip():
        s = without_units
    s = re.sub(r"\s+", "", s).rstrip(".")
    s = re.sub(r"\\leq(?![A-Za-z])", r"\\le", s)
    s = re.sub(r"\\geq(?![A-Za-z])", r"\\ge", s)
    s = re.sub(r"\\neq(?![A-Za-z])", r"\\ne", s)
    s = s.replace("bmatrix", "pmatrix")
    # "8 \cdot \pi" is "8\pi". Not before \frac, where it would make a mixed number.
    s = re.sub(r"\\(?:cdot|times)(?=[A-Za-z]|\\pi|\\sqrt)", "", s)
    # "\cot(x)" is "\cot x". Only a lone argument: "\log(x+1)" is not "\log x+1".
    s = re.sub(r"\\(sin|cos|tan|cot|sec|csc|ln|log|exp)\(([A-Za-z0-9]+)\)", r"\\\1\2", s)
    s = re.sub(r"([\^_])\{([^{}])\}", r"\1\2", s)
    s = _SLASH.sub(r"\\frac{\1}{\2}", s)
    s = _brace_arguments(s)
    choice = re.fullmatch(r"\(([A-Za-z])\)", s)   # "\text{(C)}" is C
    return choice.group(1) if choice else s


def _math_value(s):
    """The value of a plain number, fraction or mixed number, or None for anything else."""
    plain = re.fullmatch(r"(-?)(\d{1,3}(?:,\d{3})+(?:\.\d+)?|\d+(?:\.\d+)?|\.\d+)", s)
    if plain:
        value = float(plain.group(2).replace(",", ""))
        return -value if plain.group(1) else value
    fraction = re.fullmatch(r"(-?)(\d*)\\frac\{(-?\d+(?:\.\d+)?)\}\{(-?\d+(?:\.\d+)?)\}", s)
    if fraction:
        sign, whole, top, bottom = fraction.groups()
        if float(bottom) == 0:
            return None
        # MATH writes mixed numbers this way: "1\frac{4}{5}" is nine fifths.
        value = float(top) / float(bottom) + (int(whole) if whole else 0)
        return -value if sign else value
    science = re.fullmatch(r"(-?(?:\d+(?:\.\d+)?|\.\d+))\\(?:times|cdot)10\^\{?(-?\d+)\}?", s)
    if science:
        value = float("%se%s" % science.groups())
        return value if math.isfinite(value) else None
    return None


def _same_atom(p, g):
    if p == g:
        return True
    if re.fullmatch(r"[A-Za-z]{2,}", p) and p.lower() == g.lower():
        return True   # a word: "East" and "east"
    x, y = _math_value(p), _math_value(g)
    if x is not None and y is not None:
        return math.isclose(x, y, rel_tol=1e-6, abs_tol=1e-9)
    # A number with its base against the number alone: "4210_5" and "4210".
    bp, bg = _BASED.fullmatch(p), _BASED.fullmatch(g)
    if bp and not bg:
        return bp.group(1) == g
    if bg and not bp:
        return bg.group(1) == p
    return False


def _without_set_braces(s):
    """A set's braces off: "\\{1,2\\}" is the list 1,2, whose order means nothing
    either. Not "\\{1\\}\\cup\\{2\\}", which is two sets."""
    if not (s.startswith("\\{") and s.endswith("\\}")):
        return s
    depth = 0
    for i, c in enumerate(s):
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0 and i < len(s) - 1:
                return s
    return s[2:-2]


def _top_level_split(s):
    parts, depth, start = [], 0, 0
    for i, c in enumerate(s):
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "," and depth == 0:
            parts.append(s[start:i])
            start = i + 1
    parts.append(s[start:])
    return parts


def _atom_key(s):
    value = _math_value(s)
    if value is not None:
        return (0, round(value, 9), "")
    return (1, 0.0, s.lower() if re.fullmatch(r"[A-Za-z]{2,}", s) else s)


def math_equal(pred, gold):
    """Whether two MATH answers are the same once their LaTeX is normalized.

    Spacing, \\left and \\right, thin spaces, dfrac, degrees, percent and dollar
    signs, \\text{} and units, a trailing period, a leading "x =" and the short
    forms of \\frac and \\sqrt make no difference; numbers compare by value, so
    0.5 is \\frac{1}{2}; a list of answers compares in any order.
    """
    if pred is None or gold is None:
        return False
    p, g = _math_norm(pred), _math_norm(gold)
    if not p or not g:
        return False
    if p == g:
        return True
    drop_p, drop_g = not _RELATION.search(g), not _RELATION.search(p)
    if drop_p:
        p = _VARIABLE.sub("", p, count=1)
    if drop_g:
        g = _VARIABLE.sub("", g, count=1)
    p, g = _without_set_braces(p), _without_set_braces(g)
    if not p or not g:
        return False
    if _same_atom(p, g):
        return True
    # "1,-2" and "-2,1": the answers, in whatever order. Not a tuple or an
    # interval, whose order is the point.
    ps, gs = _top_level_split(p), _top_level_split(g)
    whole = ("(", "[", "{", "\\{")
    if len(ps) > 1 and len(ps) == len(gs) and not p.startswith(whole) and not g.startswith(whole):
        if drop_p:
            ps = [_VARIABLE.sub("", e, count=1) for e in ps]
        if drop_g:
            gs = [_VARIABLE.sub("", e, count=1) for e in gs]
        return sorted(map(_atom_key, ps)) == sorted(map(_atom_key, gs))
    return False


# --- code ----------------------------------------------------------------------

_FENCE_OPEN = re.compile(r"[ \t]*(`{3,}|~{3,})[ \t]*([\w+.#-]*)")
_PYTHON_TAGS = ("", "python", "python3", "py", "py3")


def _fenced_blocks(text):
    """(language, code) for each fenced block. One left open runs to the end:
    a reply cut off at its token limit still holds the code it got to."""
    blocks = []
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        opening = _FENCE_OPEN.match(lines[i])
        if not opening:
            i += 1
            continue
        fence = opening.group(1)
        closing = re.compile(r"[ \t]*" + re.escape(fence[0]) + "{%d,}[ \t]*$" % len(fence))
        body = []
        i += 1
        while i < len(lines) and not closing.match(lines[i]):
            body.append(lines[i])
            i += 1
        i += 1
        # A block inside a list item is indented with it.
        blocks.append((opening.group(2).lower(), textwrap.dedent("\n".join(body))))
    return blocks


def _defines(code, name):
    return re.search(r"^(?:async\s+)?def\s+" + re.escape(name) + r"\s*\(", code, re.MULTILINE) is not None


def code_from(text, entry_point=None):
    """The code in a model's answer: the fenced Python block that defines
    `entry_point` when one is given, else the last fenced Python block, else the
    whole text. A block with no language counts as Python."""
    if not text:
        return ""
    blocks = [code for language, code in _fenced_blocks(text) if language in _PYTHON_TAGS]
    if entry_point:
        for code in reversed(blocks):
            if _defines(code, entry_point):
                return code
    return blocks[-1] if blocks else text


_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000)

# What a program may have on any system: memory, and the size of a file it writes.
_MEMORY = 4 << 30
_FILE_SIZE = 64 << 20


def _child_limits(timeout):
    """The resource limits for the program, worked out here rather than in the
    child: between fork and exec, the less the child does the better. macOS
    refuses or ignores some of them (the address space, for one) and holds the
    rest."""
    if resource is None:
        return []
    wanted = [("RLIMIT_FSIZE", _FILE_SIZE), ("RLIMIT_AS", _MEMORY),
              # A program killed for its limits would otherwise leave a core dump.
              ("RLIMIT_CORE", 0),
              # No processes of its own. The limit counts every process the
              # user has, so at zero every fork fails: a fork bomb ends at its
              # first, and no grandchild can call setsid and outlive the group
              # a timeout kills. Linux counts threads too, so there are none of
              # those either. Root is held to no such limit.
              ("RLIMIT_NPROC", 0)]
    if timeout is not None:
        # A little over the wall clock limit, so that the clock decides, and the
        # CPU limit only catches what the clock somehow missed.
        wanted.append(("RLIMIT_CPU", int(math.ceil(timeout)) + 1))
    limits = []
    for name, value in wanted:
        which = getattr(resource, name, None)
        if which is None:
            continue
        try:
            _, hard = resource.getrlimit(which)
        except (OSError, ValueError):
            continue
        top = value + 1 if name == "RLIMIT_CPU" else value   # SIGXCPU first, SIGKILL a second later
        if hard != resource.RLIM_INFINITY:
            value, top = min(value, hard), min(top, hard)
        limits.append((which, (value, top)))
    return limits


def _apply_limits(limits):
    for which, pair in limits:
        try:
            resource.setrlimit(which, pair)
        except (OSError, ValueError):
            pass   # macOS, for one, refuses some of them


# Windows has no setrlimit. A job object stands in: one process in it and no
# more, a ceiling on the memory it commits, and whatever is in it ended when the
# last handle to the job closes, which happens however the orchestrator ends.
# Through ctypes: the standard library has no job objects, and nothing here
# installs packages.
_CREATE_SUSPENDED = 0x00000004
_JOB_LIMIT_ACTIVE_PROCESS = 0x00000008
_JOB_LIMIT_PROCESS_MEMORY = 0x00000100
_JOB_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
_JOB_EXTENDED_LIMIT_INFORMATION = 9   # the JOBOBJECTINFOCLASS


@lru_cache(maxsize=None)
def _job_limits_type():
    """JOBOBJECT_EXTENDED_LIMIT_INFORMATION, in plain ctypes types rather than
    wintypes, so that its layout can be checked on any system."""
    import ctypes

    class Basic(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64),
                    ("LimitFlags", ctypes.c_uint32), ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", ctypes.c_uint32),
                    ("Affinity", ctypes.c_size_t), ("PriorityClass", ctypes.c_uint32),
                    ("SchedulingClass", ctypes.c_uint32)]

    class Extended(ctypes.Structure):
        _fields_ = [("BasicLimitInformation", Basic), ("IoInfo", ctypes.c_uint64 * 6),
                    ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                    ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]

    return Extended


@lru_cache(maxsize=None)
def _win32():
    """kernel32 and ntdll with the prototypes used here: loaded afresh rather
    than through ctypes.windll, so that setting them changes nobody else's."""
    import ctypes
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    ntdll = ctypes.WinDLL("ntdll")
    handle = ctypes.c_void_p
    for function, result, arguments in (
            (kernel32.CreateJobObjectW, handle, (ctypes.c_void_p, ctypes.c_wchar_p)),
            (kernel32.SetInformationJobObject, ctypes.c_int,
             (handle, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32)),
            (kernel32.AssignProcessToJobObject, ctypes.c_int, (handle, handle)),
            (kernel32.CloseHandle, ctypes.c_int, (handle,)),
            # Undocumented, and how psutil resumes a process: ResumeThread wants
            # the thread's handle, which Popen does not keep.
            (ntdll.NtResumeProcess, ctypes.c_long, (handle,))):
        function.restype, function.argtypes = result, arguments
    return kernel32, ntdll


def _windows_job():
    """A job object for one program, or None when one cannot be had."""
    try:
        import ctypes
        kernel32, _ = _win32()
    except (ImportError, OSError, AttributeError):
        return None
    job = kernel32.CreateJobObjectW(None, None)
    if not job:
        return None
    limits = _job_limits_type()()
    limits.BasicLimitInformation.LimitFlags = (_JOB_LIMIT_ACTIVE_PROCESS | _JOB_LIMIT_PROCESS_MEMORY
                                               | _JOB_LIMIT_KILL_ON_JOB_CLOSE)
    limits.BasicLimitInformation.ActiveProcessLimit = 1
    # A 32-bit Python's SIZE_T cannot hold 4 GB, which is more than it has anyway.
    limits.ProcessMemoryLimit = min(_MEMORY, (1 << (8 * ctypes.sizeof(ctypes.c_size_t))) - 1)
    if not kernel32.SetInformationJobObject(job, _JOB_EXTENDED_LIMIT_INFORMATION,
                                            ctypes.byref(limits), ctypes.sizeof(limits)):
        kernel32.CloseHandle(job)
        return None
    return job


def _run_in_job(job, child):
    """Put `child`, started suspended, in `job` and let it run. False when it
    could not be let run. One the job will not take runs anyway, as it would
    have before there were jobs."""
    kernel32, ntdll = _win32()
    # Popen keeps the handle CreateProcess returned, which has every right.
    process = int(child._handle)
    kernel32.AssignProcessToJobObject(job, process)
    return ntdll.NtResumeProcess(process) >= 0   # NT_SUCCESS


def _kill_tree(child):
    """Stop the program and everything it started. A test that starts a process
    and hangs would otherwise leave that process running after the timeout."""
    if os.name == "nt":
        try:
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(child.pid)],
                           stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, creationflags=_NO_WINDOW, timeout=10)
        except (OSError, subprocess.SubprocessError):
            pass
    else:
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except OSError:
            pass
    try:
        child.kill()
    except OSError:
        pass


def _remove(folder):
    # On Windows a program that was just killed can hold its folder for a moment
    # after it has gone. Elsewhere the first try works.
    for _ in range(20):
        shutil.rmtree(folder, ignore_errors=True)
        if not os.path.exists(folder):
            return
        time.sleep(0.05)


def run_python(program, timeout=10.0, python=None):
    """Run `program` in a fresh temporary folder and say whether it passed.

    Returns (passed, detail): passed when it exits with status 0 within
    `timeout` seconds; detail is the end of what it printed, or "timed out".
    Run as `python -I -X utf8 file` with standard input closed, so nothing from the
    environment or the folder it was started from leaks in, and held to the
    limits above: no process but itself, and a ceiling on what it can take.
    """
    folder = tempfile.mkdtemp(prefix="crucible-bench-")
    job = None
    try:
        path = os.path.join(folder, "program.py")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(program or "")
        # -X utf8: on Windows a piped child would print in the ANSI code page,
        # and -I ignores PYTHONUTF8.
        command = [python or sys.executable, "-I", "-X", "utf8", path]
        options = {"cwd": folder, "stdin": subprocess.DEVNULL,
                   "stdout": subprocess.PIPE, "stderr": subprocess.STDOUT}
        if os.name == "nt":
            # The orchestrator has no console, and every program would open one.
            options["creationflags"] = _NO_WINDOW
            job = _windows_job()
            if job is not None:
                # Held until it is in the job, so it can start nothing first.
                options["creationflags"] |= _CREATE_SUSPENDED
        else:
            # Its own session, so that a timeout can kill the whole group.
            options["start_new_session"] = True
            limits = _child_limits(timeout)
            if limits:
                options["preexec_fn"] = lambda: _apply_limits(limits)
        try:
            child = subprocess.Popen(command, **options)
        except (OSError, ValueError) as error:
            return False, "could not run %s: %s" % (command[0], error)
        if job is not None and not _run_in_job(job, child):
            child.kill()
            child.communicate()
            return False, "could not start %s" % command[0]
        try:
            output, _ = child.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            _kill_tree(child)
            try:
                child.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                # Something outside the group still holds the pipe open.
                child.stdout.close()
                child.wait()
            return False, "timed out"
        output = output.decode("utf-8", "replace").replace("\r\n", "\n")
        return child.returncode == 0, output[-2000:]
    finally:
        if job is not None:
            _win32()[0].CloseHandle(job)   # and with it, anything still in it
        _remove(folder)


def _above_def(prompt, name):
    match = re.search(r"^def\s+" + re.escape(name) + r"\s*\(", prompt, re.MULTILINE)
    if match:
        return prompt[:match.start()]
    return "\n".join(line for line in prompt.splitlines() if line.startswith(("import ", "from ")))


def humaneval_program(item, completion):
    """The program that tests a HumanEval completion: the function, the item's
    `test`, and check(<entry_point>).

    A completion that defines the entry point is the whole function, below what
    the prompt has above its own def: the imports, and helpers that some tests
    call directly (HumanEval/38's test calls encode_cyclic). Any other
    completion is the body, so it goes after the prompt.
    """
    prompt = item.get("prompt") or ""
    entry = item.get("entry_point") or ""
    completion = completion or ""
    if entry and _defines(completion, entry):
        head = _above_def(prompt, entry)
        code = head.rstrip("\n") + "\n\n\n" + completion if head.strip() else completion
    else:
        code = prompt + completion
    test = (item.get("test") or "").strip("\n")
    return code.rstrip("\n") + "\n\n\n" + test + "\n\n\ncheck(" + entry + ")\n"


def mbpp_program(item, code):
    """The program that tests MBPP code: `test_imports`, the code, then each of
    the `test_list` asserts on a line of its own."""
    lines = list(item.get("test_imports") or [])
    lines.append((code or "").rstrip("\n"))
    lines.extend(item.get("test_list") or [])
    return "\n".join(lines) + "\n"


# --- IFEval: what a reply is made of -------------------------------------------

# The end of a sentence: . ! ? or an ellipsis before a space, past any closing
# quote or bracket. The CJK, Devanagari and Arabic marks need no space after.
_SENTENCE_END = re.compile(
    r"[.!?\u2026]+[\"'\u201d\u2019)\]*_]*(?=\s|$)|[\u3002\uff01\uff1f\u0964\u0965\u061f]+")

# A period after these continues the sentence: titles before a name, and Latin
# that runs on.
_RUNS_ON_AFTER = frozenset(("mr", "mrs", "ms", "dr", "prof", "sr", "jr", "st", "mt", "vs", "cf",
                            "viz", "approx", "e.g", "i.e", "eg", "ie", "fig", "figs", "eq"))
# After these it ends one only when a capital follows: "pears, etc. Then".
_ENDS_BEFORE_CAPITAL = frozenset(("etc", "inc", "ltd", "co", "corp", "al"))


def _period_runs_on(text, end):
    """Whether the period at end.start() belongs to an abbreviation, an initial
    or a list number rather than ending a sentence."""
    line_start = text.rfind("\n", 0, end.start()) + 1
    before = text[line_start:end.start()]
    last = re.search(r"(\S+)$", before)
    if not last:
        return False
    token = last.group(1).lstrip("(\"'\u201c\u2018*_[")
    lower = token.lower()
    if lower in _RUNS_ON_AFTER:
        return True
    if re.fullmatch(r"[A-Za-z]|(?:[A-Za-z]\.)+[A-Za-z]", token):
        return True   # "J. K. Rowling", "the U.S. economy", "a. the first item"
    if token.isdigit() and not before[:last.start()].strip(" \t*->#"):
        return True   # "1." numbering a list
    if lower in _ENDS_BEFORE_CAPITAL:
        return not text[end.end():].lstrip()[:1].isupper()
    return False


def _sentence_count(text):
    count, start = 0, 0
    for end in _SENTENCE_END.finditer(text):
        mark = end.group()
        if mark.startswith(".") and not mark.startswith("..") and _period_runs_on(text, end):
            continue
        if _has_alnum(text[start:end.end()]):
            count += 1
        start = end.end()
    return count + (1 if _has_alnum(text[start:]) else 0)


def _word_count(text):
    # What a word processor counts: runs between spaces with a letter or digit
    # in them. "don't" is one word, a lone "-" is none.
    return sum(1 for token in text.split() if _has_alnum(token))


def _occurrences(text, word):
    """Whole words, any case: "war" is not in "software", nor "the" in "they"."""
    word = str(word).strip()
    if not word:
        return 0
    return len(re.findall(r"(?<!\w)" + re.escape(word) + r"(?!\w)", text, re.IGNORECASE))


def _star_paragraphs(text):
    """The paragraphs between *** dividers, or None when two dividers have
    nothing between them. A divider stands apart: "***bold italic***" is not one."""
    parts = [part.strip() for part in re.split(r"(?<!\S)\*{3,}(?!\S)", text)]
    while parts and not _has_alnum(parts[0]):
        parts.pop(0)
    while parts and not _has_alnum(parts[-1]):
        parts.pop()
    if any(not _has_alnum(part) for part in parts):
        return None
    return parts


def _blank_line_paragraphs(text):
    return [part for part in re.split(r"\n[ \t]*\n", text.strip()) if _has_alnum(part)]


# *highlighted*, **bold** or ***both***: opened by asterisks not followed by a
# space and closed by ones not preceded by one, so a "* " bullet opens nothing.
_HIGHLIGHT = re.compile(r"(?<![*\w])(\*{1,3})(?=[^\s*])([^*\n]+?)(?<=[^\s*])\1(?![*\w])")

# "* point" or "- point". Not a line of nothing but stars and dashes.
_BULLET = re.compile(r"^[ \t]*[*-][ \t]+(?![*\- \t]*$)\S", re.MULTILINE)

_CONSTRAINED = ("My answer is yes.", "My answer is no.", "My answer is maybe.")

_CAPITAL_WORD = re.compile(r"[^\W\d_]+(?:['\u2019-][^\W\d_]+)*")


# --- IFEval: languages ---------------------------------------------------------

# Which script each language is written in, by ISO 639-1 code.
_SCRIPT_LANGUAGES = {
    "ARABIC": "ar fa ur ps sd ug ku ckb", "HEBREW": "he yi", "GREEK": "el",
    "CYRILLIC": "ru bg uk sr mk be kk ky mn tg", "ARMENIAN": "hy", "GEORGIAN": "ka",
    "DEVANAGARI": "hi mr ne sa", "BENGALI": "bn as", "GURMUKHI": "pa", "GUJARATI": "gu",
    "ORIYA": "or", "TAMIL": "ta", "TELUGU": "te", "KANNADA": "kn", "MALAYALAM": "ml",
    "SINHALA": "si", "THAI": "th", "LAO": "lo", "MYANMAR": "my", "KHMER": "km",
    "TIBETAN": "bo", "ETHIOPIC": "am ti", "HANGUL": "ko",
    "LATIN": ("en de nl fr es ca pt it ro sv da no fi et pl cs sk hu tr id sw vi tl hr "
              "sl lv lt sq af so cy eu gl ga is mt ms eo la ha yo zu xh ig uz az tk"),
}
_SCRIPT_OF = {code: script for script, codes in _SCRIPT_LANGUAGES.items() for code in codes.split()}
_ALIASES = {"iw": "he", "in": "id", "ji": "yi", "nb": "no", "nn": "no", "fil": "tl"}

# Common short words, for telling apart languages that share a script. Each is
# weighted by how many languages share it, so "de" counts a little for several
# and "the" a lot for English.
_WORDS = {
    "en": "the and of to is are was were that this with for you your not have has but they "
          "their what which would there from been will can about when",
    "de": "der die das und ist nicht ein eine einen zu den dem mit sich auf für von auch ich "
          "sie wir es aber oder wenn dass noch nur bei aus sind wird werden kann haben hat durch "
          "über nach sehr kein keine diese dieser",
    "nl": "de het een en van is dat op te voor met zijn niet aan er maar om ook als dan bij uit "
          "nog wel naar kan ik wij hij zij deze dit wordt worden heeft hebben geen meer door veel",
    "fr": "le la les des du un une et est que qui dans pour pas sur au aux avec ce cette ces il "
          "elle ils nous vous je ne se sont mais ou plus par son sa ses leur être avoir fait comme "
          "tout très aussi était peut été sans",
    "es": "el la los las del que y en un una es por con no para se su sus lo como más pero al le "
          "ya este esta muy también hay porque cuando sin sobre entre son está ser tiene todo nos "
          "qué cómo donde puede",
    "ca": "el la els les del que i en un una és per amb no als com més però aquest aquesta molt "
          "també hi ha són està ser tot quan perquè sense sobre entre seu seva",
    "pt": "o a os as do da dos das que e é um uma em no na nos nas para com não por mais se como "
          "mas ao ele ela eles seu sua ou quando muito já também só pelo pela até isso isto esse "
          "essa este esta você são está foi tem ser há",
    "it": "il lo la gli le di del della dei delle che e è un una uno per non con si da in al alla "
          "sono come più ma anche questo questa molto nel nella ha hanno essere tutto quando perché "
          "se ci suo sua loro cosa solo sul",
    "ro": "și şi în de la cu pe un o este sunt care că nu se mai din pentru ca ce sau dar au al "
          "ale lui ei el ea acest această foarte fi fost până după când dacă prin despre doar tot",
    "sv": "och att det som en ett på är av för med till den har de inte om var jag du han hon vi "
          "så men eller från kan ska skulle efter också när sig sin sitt mycket bara alla finns "
          "detta dessa där här nu vad",
    "da": "og at det som en et på er af for med til den har de ikke om var jeg du han hun vi så "
          "men eller fra kan skal efter også når sig sin sit meget kun alle findes denne disse der "
          "her nu være blev have hvad",
    "no": "og at det som en et på er av for med til den har de ikke om var jeg du han hun vi så "
          "men eller fra kan skal etter også når seg sin sitt mye bare alle finnes denne disse der "
          "her nå være ble hva",
    "fi": "ja on ei se että hän oli ole kun niin mutta tai jos kuin ovat myös vain tämä joka mitä "
          "sitä siitä minä sinä me te he kanssa voi jo nyt sen hänen kaikki sekä eli vaan mikä "
          "tämän nämä olla ollut koska paljon hyvin",
    "et": "ja on ei see et kui ka oli ole mis aga või nii kes siis veel väga ning oma seda selle "
          "need mina sina tema meie teie nemad kõik juba nüüd ainult",
    "pl": "i w na z nie się że do to jest jak o ale co po od za przez dla tak czy są być jego jej "
          "ich oraz który która które ten ta te tym tego może bardzo już tylko jeszcze gdy lub "
          "ponieważ także też bez przy",
    "cs": "a v na se je že do to s z o i ale jak pro po od za k by jsou být jeho její jejich "
          "který která které ten ta tento tato toto může velmi už jen ještě když nebo protože "
          "také bez při tak co aby není",
    "sk": "a v na sa je že do to s z o i ale ako pre po od za k by sú byť jeho jej ich ktorý "
          "ktorá ktoré ten táto toto môže veľmi už len ešte keď alebo pretože tiež bez pri tak "
          "čo aby nie",
    "hu": "a az és hogy nem is egy van volt meg de ez azt mint csak már még el ki be fel le "
          "vagy kell lehet mert ha nagyon minden akkor után között pedig ami amely aki én te ő "
          "mi ti ők ezt itt ott most",
    "tr": "ve bir bu da de için ile çok ne daha gibi ama en mi mı o şu ben sen biz siz onlar var "
          "yok olarak olan kadar sonra değil ki her hem veya ya diye şey nasıl neden çünkü ancak "
          "bütün tüm",
    "id": "yang dan di ke dari ini itu dengan untuk tidak ada akan juga pada adalah saya kami "
          "kita mereka anda dia ia bisa dapat sudah telah oleh karena atau tetapi jika sangat "
          "lebih seperti dalam bahwa hanya banyak harus satu",
    "sw": "na ya wa za kwa ni la katika hii huo hiyo kuwa lakini au pia sana yake wake kama hata "
          "tu ana kila bila baada kwamba mimi wewe yeye sisi nyinyi wao nini ndiyo hapana si cha "
          "vya hiki hicho huu ili kuna watu mtu",
    "vi": "và của là có không những các được trong cho này một người với để đã khi như thì "
          "cũng rất nhưng từ đến về bạn tôi chúng sẽ ra làm mà nào đó theo nhiều hay vì nếu",
    "tl": "ang ng sa na at mga ay ko mo siya ako ikaw kami tayo sila ito iyon para hindi may "
          "mayroon kung pero dahil lang din rin po naman ba nga kanyang kanila natin namin niya",
    "hr": "i je u na da se za su od s sa a ne kao što koji koja koje to ali ili iz po bi će sam "
          "smo ste biti bio bila ovo taj ta te jer kada samo već još vrlo može treba",
    "hi": "है हैं का की के में और से को यह नहीं एक पर भी था थे थी हो कर लिए वह जो तो कि गया "
          "रहा सकते अपने क्या होता होती हुए आप इस उस",
    "mr": "आहे आहेत आणि च्या मध्ये नाही हे ते या करा होते एक की पण तर तो ती त्या आपण म्हणून "
          "साठी केले असे कसे काय आम्ही तुम्ही ला चा ची चे",
    "ne": "छ छन् हो र को मा पनि गर्न भएको हुन्छ थियो यो त्यो लागि गरेको हुन गर्छ गर्दै तथा भने "
          "छैन हामी तपाईं तिमी उनी सक्छ थिए एउटा गरी",
    "ru": "и в не на что это как с по но из для он она они мы вы я так же все его к у от был "
          "была были только если или когда уже бы этот эти очень можно нужно",
    "bg": "е са на и за да се от с не това като но ще по който която които във със при или "
          "когато ако си ги го му тя той те ние вие аз тези много може трябва беше бяха няма има",
    "uk": "і в не на що це як з до та але для він вона вони ми ви я також ще вже був була були "
          "тільки якщо або коли дуже можна треба є його",
}


def _index_words():
    index = {}
    for language, words in _WORDS.items():
        for word in set(words.split()):
            index.setdefault(word, []).append(language)
    return index


_WORD_INDEX = _index_words()

# Letters only Urdu uses, of the Arabic script: tteh, ddal, rreh, noon ghunna,
# yeh barree, heh goal, heh doachashmee.
_URDU_LETTERS = frozenset("\u0679\u0688\u0691\u06ba\u06d2\u06c1\u06be")
# Persian's (and Urdu's): peh, tcheh, jeh, gaf, Farsi yeh, keheh.
_PERSIAN_LETTERS = frozenset("\u067e\u0686\u0698\u06af\u06cc\u06a9")
# Arabic's own: teh marbuta, Arabic yeh, Arabic kaf, alef maksura.
_ARABIC_LETTERS = frozenset("\u0629\u064a\u0643\u0649")


@lru_cache(maxsize=None)
def _script(c):
    """The script a letter belongs to, from the first word of its Unicode name."""
    try:
        name = unicodedata.name(c)
    except ValueError:
        return ""
    first, _, rest = name.partition(" ")
    if first == "CJK":
        return "HAN"
    if first in ("HIRAGANA", "KATAKANA", "KATAKANA-HIRAGANA"):
        return "KANA"
    if first in ("HALFWIDTH", "FULLWIDTH"):
        second = rest.split(" ", 1)[0]
        return "KANA" if second == "KATAKANA" else second
    return first


def _tokens(text):
    # Letters and the marks on them. Python's \w leaves the marks out, and they
    # are half of every Devanagari word.
    words, word = [], []
    for c in text:
        if unicodedata.category(c)[0] in "LM":
            word.append(c)
        elif word:
            words.append("".join(word).lower())
            word = []
    if word:
        words.append("".join(word).lower())
    return words


def _word_scores(tokens):
    scores = {}
    for token in tokens:
        languages = _WORD_INDEX.get(token)
        if languages:
            for language in languages:
                scores[language] = scores.get(language, 0.0) + 1.0 / len(languages)
    return scores


def _arabic_script_language(text, letters):
    urdu = sum(c in _URDU_LETTERS for c in text)
    persian = sum(c in _PERSIAN_LETTERS for c in text)
    arabic = sum(c in _ARABIC_LETTERS for c in text)
    if urdu >= 0.02 * letters:
        return "ur"
    return "fa" if persian > arabic else "ar"


def _in_language(text, code):
    """Whether `text` is mostly in the language with ISO 639-1 `code`.

    A heuristic, not a language detector. Most of the letters have to be in the
    language's script. Where a script is shared, the language has to win a
    count of its common short words against the others with that script (Hindi
    against Marathi, Russian against Bulgarian, Italian against Spanish), and
    Arabic, Persian and Urdu go by the letters only each of them uses. A line
    or two with none of the counted words is given the benefit of the doubt; a
    paragraph is not. Codes the tables do not know are not followed: there is
    no way to tell.
    """
    code = str(code or "").strip().lower().replace("_", "-").split("-")[0]
    code = _ALIASES.get(code, code)
    text = unicodedata.normalize("NFC", text)
    counts = {}
    for c in text:
        if unicodedata.category(c).startswith("L"):
            script = _script(c)
            counts[script] = counts.get(script, 0) + 1
    letters = sum(counts.values())
    if not letters:
        return False
    if code in ("zh", "ja"):
        han, kana = counts.get("HAN", 0), counts.get("KANA", 0)
        if (han + kana) * 2 <= letters:
            return False
        # Japanese mixes kana into its kanji; Chinese has none.
        kana_share = kana / (han + kana)
        return kana_share >= 0.05 if code == "ja" else kana_share < 0.05
    script = _SCRIPT_OF.get(code)
    if script is None or counts.get(script, 0) * 2 <= letters:
        return False
    if script == "ARABIC":
        return code not in ("ar", "fa", "ur") or _arabic_script_language(text, counts[script]) == code
    tokens = _tokens(text)
    scores = {language: score for language, score in _word_scores(tokens).items()
              if _SCRIPT_OF.get(language) == script}
    if code in _WORDS:
        if not scores.get(code):
            return not scores and len(tokens) < 12
        return scores[code] >= max(scores.values())
    if script != "LATIN":
        return True
    # A Latin-script language with no word list: at least not English, which is
    # what a model asked for another language most often writes instead.
    others = max((score for language, score in scores.items() if language != "en"), default=0.0)
    return scores.get("en", 0.0) <= others


# --- IFEval: the instructions --------------------------------------------------


def _number(args, name):
    value = args[name]
    if isinstance(value, bool):
        raise ValueError(name)
    return int(float(value))


def _compare(count, relation, n):
    relation = str(relation).strip().lower()
    if relation == "less than":
        return count < n
    if relation == "at least":
        return count >= n
    raise ValueError(relation)


def _keywords_exist(text, args, prompt):
    return all(_occurrences(text, word) > 0 for word in args["keywords"])


def _keyword_frequency(text, args, prompt):
    return _compare(_occurrences(text, args["keyword"]), args["relation"], _number(args, "frequency"))


def _forbidden_words(text, args, prompt):
    return not any(_occurrences(text, word) for word in args["forbidden_words"])


def _letter_frequency(text, args, prompt):
    letter = str(args["letter"])
    if not letter:
        return False
    count = text.lower().count(letter.lower())
    return _compare(count, args["let_relation"], _number(args, "let_frequency"))


def _response_language(text, args, prompt):
    return _in_language(text, args["language"])


def _number_sentences(text, args, prompt):
    return _compare(_sentence_count(text), args["relation"], _number(args, "num_sentences"))


def _number_paragraphs(text, args, prompt):
    paragraphs = _star_paragraphs(text)
    return paragraphs is not None and len(paragraphs) == _number(args, "num_paragraphs")


def _number_words(text, args, prompt):
    return _compare(_word_count(text), args["relation"], _number(args, "num_words"))


def _nth_paragraph_first_word(text, args, prompt):
    paragraphs = _blank_line_paragraphs(text)
    if len(paragraphs) != _number(args, "num_paragraphs"):
        return False
    first = str(args.get("first_word") or "").strip().lower()
    if not first or args.get("nth_paragraph") is None:
        return True
    nth = _number(args, "nth_paragraph")
    if not 1 <= nth <= len(paragraphs):
        return False
    word = re.match(r"\W*(\w+)", paragraphs[nth - 1])
    return word is not None and word.group(1).lower() == first


def _number_placeholders(text, args, prompt):
    found = [inside for inside in re.findall(r"\[([^\[\]\n]*)\]", text) if inside.strip()]
    return len(found) >= _number(args, "num_placeholders")


def _postscript(text, args, prompt):
    marker = str(args["postscript_marker"]).strip()
    parts = [re.escape(part) for part in re.split(r"\s*\.\s*", marker) if part]
    if not parts:
        return False
    # "P.P.S" is also "P. P. S." and "p.p.s": the case goes with lowercase replies.
    pattern = r"\.\s*".join(parts) + (r"\." if marker.endswith(".") else "")
    return re.search(r"^[ \t>#*_]*" + pattern, text, re.IGNORECASE | re.MULTILINE) is not None


def _number_bullet_lists(text, args, prompt):
    return len(_BULLET.findall(text)) == _number(args, "num_bullets")


def _constrained_response(text, args, prompt):
    # One of them, not a hedge across two.
    return sum(1 for option in _CONSTRAINED if option in text) == 1


def _number_highlighted_sections(text, args, prompt):
    return len(_HIGHLIGHT.findall(text)) >= _number(args, "num_highlights")


def _multiple_sections(text, args, prompt):
    spliter = str(args["section_spliter"]).strip()
    if not spliter:
        return False
    found = re.findall(re.escape(spliter) + r"\s*\d+", text)
    return len(found) >= _number(args, "num_sections")


def _json_format(text, args, prompt):
    body = text.strip()
    if len(body) >= 6 and body.startswith("```") and body.endswith("```"):
        body = re.sub(r"^[ \t]*json\b", "", body[3:-3], flags=re.IGNORECASE)
    try:
        json.loads(body)
    except (ValueError, RecursionError):
        return False
    return True


def _title(text, args, prompt):
    return any(title.strip() for title in re.findall(r"<<([^\n]*?)>>", text))


def _two_responses(text, args, prompt):
    parts = [part.strip() for part in re.split(r"\*{6,}", text)]
    while parts and not parts[0]:
        parts.pop(0)
    while parts and not parts[-1]:
        parts.pop()
    return len(parts) == 2 and all(parts) and parts[0] != parts[1]


def _repeat_prompt(text, args, prompt):
    target = " ".join(str(args.get("prompt_to_repeat") or prompt or "").split())
    return bool(target) and " ".join(text.split()).startswith(target)


def _end_checker(text, args, prompt):
    # Any case: the same prompt can ask for all capitals and a phrase in mixed case.
    phrase = str(args["end_phrase"]).strip().lower()
    return bool(phrase) and text.strip().lower().endswith(phrase)


def _quotation(text, args, prompt):
    body = text.strip()
    return len(body) >= 2 and (body[0], body[-1]) in (('"', '"'), ("\u201c", "\u201d"))


def _capital_word_frequency(text, args, prompt):
    # Words of two letters or more: "I" is always a capital, and counting it
    # would fail every "fewer than four" reply that says "I".
    count = sum(1 for word in _CAPITAL_WORD.findall(text)
                if word.isupper() and sum(c.isalpha() for c in word) >= 2)
    return _compare(count, args["capital_relation"], _number(args, "capital_frequency"))


def _english_capital(text, args, prompt):
    return text.isupper() and _in_language(text, "en")


def _english_lowercase(text, args, prompt):
    return text.islower() and _in_language(text, "en")


def _no_comma(text, args, prompt):
    # The Arabic, ideographic and full-width commas are commas too.
    return not any(c in text for c in ",\u060c\u3001\uff0c")


_INSTRUCTIONS = {
    "keywords:existence": _keywords_exist,
    "keywords:frequency": _keyword_frequency,
    "keywords:forbidden_words": _forbidden_words,
    "keywords:letter_frequency": _letter_frequency,
    "language:response_language": _response_language,
    "length_constraints:number_sentences": _number_sentences,
    "length_constraints:number_paragraphs": _number_paragraphs,
    "length_constraints:number_words": _number_words,
    "length_constraints:nth_paragraph_first_word": _nth_paragraph_first_word,
    "detectable_content:number_placeholders": _number_placeholders,
    "detectable_content:postscript": _postscript,
    "detectable_format:number_bullet_lists": _number_bullet_lists,
    "detectable_format:constrained_response": _constrained_response,
    "detectable_format:number_highlighted_sections": _number_highlighted_sections,
    "detectable_format:multiple_sections": _multiple_sections,
    "detectable_format:json_format": _json_format,
    "detectable_format:title": _title,
    "combination:two_responses": _two_responses,
    "combination:repeat_prompt": _repeat_prompt,
    "startend:end_checker": _end_checker,
    "startend:quotation": _quotation,
    "change_case:capital_word_frequency": _capital_word_frequency,
    "change_case:english_capital": _english_capital,
    "change_case:english_lowercase": _english_lowercase,
    "punctuation:no_comma": _no_comma,
}


def ifeval_follows(response, instruction_ids, kwargs, prompt=None):
    """Whether `response` follows each instruction, strictly: one bool per id.

    `kwargs` lines up with `instruction_ids`, one dict each; None values in
    them are ignored. An id this does not know, or one missing what it needs,
    counts as not followed.
    """
    text = (response or "").replace("\r\n", "\n")
    # Nothing said follows nothing. "No commas" and "all lowercase" hold of an
    # empty reply, which is what an answer cut off while the model was still
    # thinking leaves: gpt-oss scored two of those as followed.
    if not text.strip():
        return [False for _ in instruction_ids or []]
    results = []
    for i, instruction in enumerate(instruction_ids or []):
        args = kwargs[i] if kwargs and i < len(kwargs) else None
        if isinstance(args, str):
            try:
                args = json.loads(args)
            except ValueError:
                args = None
        args = {k: v for k, v in args.items() if v is not None} if isinstance(args, dict) else {}
        check = _INSTRUCTIONS.get(str(instruction).strip())
        try:
            results.append(bool(check(text, args, prompt)) if check else False)
        except (KeyError, TypeError, ValueError):
            results.append(False)
    return results


def ifeval_score(results):
    """(prompt level, instruction level) accuracy from one list of bools per
    prompt. A prompt counts when every instruction in it was followed."""
    results = [list(r or []) for r in results or []]
    if not results:
        return 0.0, 0.0
    prompt_level = sum(1 for r in results if r and all(r)) / len(results)
    followed = [x for r in results for x in r]
    instruction_level = sum(1 for x in followed if x) / len(followed) if followed else 0.0
    return float(prompt_level), float(instruction_level)
